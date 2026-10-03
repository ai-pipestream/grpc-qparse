#include "lzw_filter.h"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <qpdf/Pipeline.hh>
#include <qpdf/Pl_Buffer.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFStreamFilter.hh>

namespace grpc_qparse {
namespace {

constexpr unsigned kClearCode = 256;
constexpr unsigned kEndCode = 257;
constexpr unsigned kFirstEntry = 258;
constexpr unsigned kMaxCodes = 4096;

// The /LZWDecode algorithm (ISO 32000-1, 7.4.4): codes of 9 to 12 bits,
// most significant bit first, a table that grows by one string per code
// and a clear code that empties it. Throws once the decoded output passes
// limit, before the bytes past it are written on.
class LimitedLzwDecoder final : public Pipeline {
 public:
  LimitedLzwDecoder(Pipeline* next, bool early_change, uint64_t limit)
      : Pipeline("limited lzw decode", next),
        early_change_(early_change ? 1 : 0),
        limit_(limit) {}

  void write(unsigned char const* data, size_t len) override {
    for (size_t i = 0; i < len && !ended_; ++i) {
      bits_ = (bits_ << 8) | data[i];
      bit_count_ += 8;
      // Codes are at least 9 bits, so a byte completes at most one.
      if (bit_count_ >= code_size_) {
        bit_count_ -= code_size_;
        HandleCode((bits_ >> bit_count_) & ((1U << code_size_) - 1U));
      }
    }
  }

  void finish() override { next()->finish(); }

 private:
  // The string a code stands for; the caller has checked the code.
  std::string StringOf(unsigned code) const {
    if (code < 256) return std::string(1, static_cast<char>(code));
    return table_[code - kFirstEntry];
  }

  void HandleCode(unsigned code) {
    if (code == kClearCode) {
      table_.clear();
      code_size_ = 9;
      last_code_ = kClearCode;
      return;
    }
    if (code == kEndCode) {
      ended_ = true;
      return;
    }
    const size_t known = table_.size();
    if (code >= kFirstEntry && code - kFirstEntry > known) {
      throw std::runtime_error("LZWDecode: code past the table");
    }
    if (last_code_ != kClearCode) {
      // The encoder added the last code's string plus the first byte of
      // this one. A code naming that very entry starts with the last
      // code's first byte.
      std::string entry = StringOf(last_code_);
      entry.push_back(code >= kFirstEntry && code - kFirstEntry == known
                          ? entry[0]
                          : StringOf(code)[0]);
      const unsigned index = kFirstEntry + static_cast<unsigned>(known);
      if (index == kMaxCodes) throw std::runtime_error("LZWDecode: table full");
      table_.push_back(std::move(entry));
      const unsigned change_at = index + early_change_;
      if (change_at == 511 || change_at == 1023 || change_at == 2047) {
        ++code_size_;
      }
    } else if (code >= kFirstEntry) {
      throw std::runtime_error("LZWDecode: code past the table");
    }
    const std::string out = StringOf(code);
    decoded_ += out.size();
    if (decoded_ > limit_) {
      throw std::runtime_error("LZWDecode: stream decodes to more than " +
                               std::to_string(limit_) + " bytes");
    }
    next()->write(reinterpret_cast<unsigned char const*>(out.data()),
                  out.size());
    last_code_ = code;
  }

  const unsigned early_change_;
  const uint64_t limit_;
  uint32_t bits_ = 0;
  unsigned bit_count_ = 0;
  unsigned code_size_ = 9;
  unsigned last_code_ = kClearCode;
  bool ended_ = false;
  uint64_t decoded_ = 0;
  std::vector<std::string> table_;
};

// Applies a PNG or TIFF predictor to the decoded LZW bytes with qpdf's own
// code, which qpdf keeps private: the bytes are deflated and handed to a
// scratch /FlateDecode stream with the same /DecodeParms. Inflating runs
// under Pl_Flate's limit, and a predictor never adds bytes.
class PredictorStage final : public Pipeline {
 public:
  PredictorStage(Pipeline* next, QPDFObjectHandle parms)
      : Pipeline("lzw predictor", next),
        parms_(parms),
        buffer_("lzw predictor buffer"),
        deflate_("lzw predictor deflate", &buffer_, Pl_Flate::a_deflate) {}

  void write(unsigned char const* data, size_t len) override {
    deflate_.write(data, len);
  }

  void finish() override {
    deflate_.finish();
    QPDF scratch;
    scratch.emptyPDF();
    QPDFObjectHandle stream = QPDFObjectHandle::newStream(&scratch);
    stream.replaceStreamData(buffer_.getBufferSharedPointer(),
                             QPDFObjectHandle::newName("/FlateDecode"),
                             parms_);
    // Asked with no pipeline, pipeStreamData only says whether it can
    // decode; with one, it would pass undecodable bytes on as they are.
    bool filtered = false;
    if (!stream.pipeStreamData(nullptr, 0, qpdf_dl_generalized) ||
        !stream.pipeStreamData(next(), &filtered, 0, qpdf_dl_generalized) ||
        !filtered) {
      throw std::runtime_error("LZWDecode: the predictor failed");
    }
  }

 private:
  QPDFObjectHandle parms_;
  Pl_Buffer buffer_;
  Pl_Flate deflate_;
};

class LimitedLzwFilter final : public QPDFStreamFilter {
 public:
  explicit LimitedLzwFilter(uint64_t limit) : limit_(limit) {}

  // The parameters qpdf's own /LZWDecode accepts, checked the same way.
  bool setDecodeParms(QPDFObjectHandle parms) override {
    if (parms.isNull()) return true;
    if (!parms.isDictionary()) return false;
    int columns = 0;
    for (const std::string& key : parms.getKeys()) {
      QPDFObjectHandle value = parms.getKey(key);
      const bool predictor_key = key == "/Predictor" || key == "/Columns" ||
                                 key == "/Colors" ||
                                 key == "/BitsPerComponent";
      if (!predictor_key && key != "/EarlyChange") continue;
      if (!value.isInteger()) return false;
      const long long number = value.getIntValue();
      if (key == "/EarlyChange") {
        if (number != 0 && number != 1) return false;
        early_change_ = number == 1;
        continue;
      }
      if (key == "/Predictor") {
        if (!(number == 1 || number == 2 || (number >= 10 && number <= 15))) {
          return false;
        }
        predictor_ = static_cast<int>(number);
      } else if (key == "/Columns") {
        columns = static_cast<int>(number);
      }
      predictor_parms_.replaceKey(key, QPDFObjectHandle::newInteger(number));
    }
    return predictor_ <= 1 || columns != 0;
  }

  Pipeline* getDecodePipeline(Pipeline* next) override {
    if (predictor_ > 1) {
      predictor_stage_ = std::make_unique<PredictorStage>(next, predictor_parms_);
      next = predictor_stage_.get();
    }
    decoder_ = std::make_unique<LimitedLzwDecoder>(next, early_change_, limit_);
    return decoder_.get();
  }

 private:
  const uint64_t limit_;
  bool early_change_ = true;
  int predictor_ = 1;
  QPDFObjectHandle predictor_parms_ = QPDFObjectHandle::newDictionary();
  std::unique_ptr<PredictorStage> predictor_stage_;
  std::unique_ptr<LimitedLzwDecoder> decoder_;
};

}  // namespace

void InstallLimitedLzwDecode(uint64_t limit_bytes) {
  QPDF::registerStreamFilter("/LZWDecode", [limit_bytes]() {
    return std::make_shared<LimitedLzwFilter>(limit_bytes);
  });
}

}  // namespace grpc_qparse
