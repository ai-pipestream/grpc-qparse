#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "ai/protomolt/parse/pdf/v1/pdf_backend_service.grpc.pb.h"
#include "call_slots.h"
#include "document_cache.h"
#include "parse_stream_session.h"

namespace grpc_qparse {

// The most bytes one Flate, RunLength or LZW stream may decode to, by default:
// 512 MiB, the samples of a 2^27-pixel image (the largest Render draws) at
// four bytes a pixel. qpdf decodes a stream whole before anyone sees its
// size, so this is what bounds a compressed stream that inflates to far
// more than it declares.
inline constexpr uint64_t kMaxDecodedStreamBytes = uint64_t{1} << 29;

// GRPC_QPARSE_MAX_DECODED_STREAM_BYTES; an unset, unparseable or zero
// value keeps kMaxDecodedStreamBytes.
uint64_t DecodedStreamLimitFromEnv();

// One-time process setup: engine logging, the font resource directory, and
// the decoded stream limit, which qpdf holds process-wide.
void InitEngine(const std::string& resources_dir,
                uint64_t decoded_stream_limit = DecodedStreamLimitFromEnv());

// The largest raster Render produces, by default and at most: 2^27 pixels,
// a 512 MiB RGBA canvas, the most that still fits one message under the
// fleet's 520 MiB limit.
inline constexpr uint64_t kMaxRenderPixels = uint64_t{1} << 27;

// Bounds on Render. A page whose raster at the requested DPI would exceed
// max_pixels, or 65535 pixels on a side (the rasterizer's own limit),
// fails the call with RESOURCE_EXHAUSTED before any page is rendered. An
// image XObject with more than max_pixels pixels, or whose soft mask has,
// is never decoded, and the page that draws it fails with
// RESOURCE_EXHAUSTED.
struct RenderLimits {
  // GRPC_QPARSE_RENDER_MAX_PIXELS, at most kMaxRenderPixels.
  uint64_t max_pixels = kMaxRenderPixels;
};

// Reads the Render bounds from the environment; an unset, unparseable or
// zero value keeps the default, a larger one is capped.
RenderLimits RenderLimitsFromEnv();

// PdfBackendService over the MIT qpdf-based cell parser. The engine is
// safe to use concurrently through independent per-request decoder
// instances, so the service is plain thread-per-request with no worker
// pool; CallSlots bounds how many Parse and Render calls run at once. The
// content-addressed byte cache behind PdfDocument.sha256 lives here in the
// one server process, shared across requests.
class QparseServiceImpl final
    : public ai::protomolt::parse::pdf::v1::PdfBackendService::Service {
 public:
  // Cache, Render and call bounds from the GRPC_QPARSE_* environment.
  QparseServiceImpl()
      : QparseServiceImpl(DocumentCacheConfigFromEnv(), RenderLimitsFromEnv()) {}
  explicit QparseServiceImpl(DocumentCacheConfig cache_config,
                             RenderLimits render_limits = RenderLimitsFromEnv(),
                             CallLimits call_limits = CallLimitsFromEnv())
      : cache_(cache_config),
        render_limits_(render_limits),
        slots_(call_limits) {}
  grpc::Status Probe(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ProbeRequest* request,
      ai::protomolt::parse::pdf::v1::ProbeResponse* response) override;

  grpc::Status Parse(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ParseRequest* request,
      grpc::ServerWriter<ai::protomolt::parse::pdf::v1::ParseResponse>*
          writer) override;

  grpc::Status Render(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::RenderRequest* request,
      grpc::ServerWriter<ai::protomolt::parse::pdf::v1::RenderResponse>*
          writer) override;

  grpc::Status GetServiceInfo(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ServiceInfoRequest* request,
      ai::protomolt::parse::pdf::v1::ServiceInfoResponse* response) override;

  // The bidirectional session (parse_stream_session.h): the chunked upload
  // is assembled once, and the open, every ParsePages and every RenderPages
  // are served by the same code as Parse and Render, on the stream's bytes
  // with no re-upload and no cache lookup. Each request takes a call slot
  // like a unary call.
  grpc::Status ParseStream(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<
          ai::protomolt::parse::pdf::v1::ParseStreamResponse,
          ai::protomolt::parse::pdf::v1::ParseStreamRequest>* stream) override;

  // Pages the engine has decoded since the service started, across Parse
  // and Render. A call decodes only the pages its range names; tests read
  // this to prove it.
  uint64_t decoded_pages() const { return decoded_pages_.load(); }

 private:
  // Parse and Render to any sink. stream_bytes, when set, are the document
  // (a ParseStream's upload) and request->document() supplies only the
  // password; otherwise the document resolves through the handshake.
  grpc::Status ParseTo(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::ParseRequest* request,
      const std::shared_ptr<std::string>& stream_bytes,
      MessageSink<ai::protomolt::parse::pdf::v1::ParseResponse>* writer);
  grpc::Status RenderTo(
      grpc::ServerContext* context,
      const ai::protomolt::parse::pdf::v1::RenderRequest* request,
      const std::shared_ptr<std::string>& stream_bytes,
      MessageSink<ai::protomolt::parse::pdf::v1::RenderResponse>* writer);

  DocumentCache cache_;
  const RenderLimits render_limits_;
  CallSlots slots_;
  std::atomic<uint64_t> decoded_pages_{0};
};

}  // namespace grpc_qparse
