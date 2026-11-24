#include "valid_utf8.h"

#include <cstddef>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <google/protobuf/descriptor.h>

namespace grpc_qparse {

namespace {

// Length of the well-formed UTF-8 sequence at s[i] (the Unicode standard's
// table 3-7: no overlong forms, no surrogates, nothing above U+10FFFF), or
// 0 when the bytes there do not form one.
size_t SequenceLength(std::string_view s, size_t i) {
  const auto byte = [&s](size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char lead = byte(i);
  if (lead < 0x80) return 1;
  size_t length = 0;
  unsigned char low = 0x80;
  unsigned char high = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead == 0xE0) {
    length = 3;
    low = 0xA0;
  } else if (lead == 0xED) {
    length = 3;
    high = 0x9F;
  } else if (lead >= 0xE1 && lead <= 0xEF) {
    length = 3;
  } else if (lead == 0xF0) {
    length = 4;
    low = 0x90;
  } else if (lead >= 0xF1 && lead <= 0xF3) {
    length = 4;
  } else if (lead == 0xF4) {
    length = 4;
    high = 0x8F;
  } else {
    return 0;
  }
  if (i + length > s.size()) return 0;
  if (byte(i + 1) < low || byte(i + 1) > high) return 0;
  for (size_t k = 2; k < length; ++k) {
    if (byte(i + k) < 0x80 || byte(i + k) > 0xBF) return 0;
  }
  return length;
}

// Whether a message of this type can hold a string, itself or in a
// message inside it. `entered` is the types already being looked at, so a
// type that holds itself (an outline node's children) ends the search.
bool ReachesString(const google::protobuf::Descriptor* type,
                   std::set<const google::protobuf::Descriptor*>* entered) {
  using google::protobuf::FieldDescriptor;
  if (!entered->insert(type).second) return false;
  for (int i = 0; i < type->field_count(); ++i) {
    const FieldDescriptor* field = type->field(i);
    if (field->type() == FieldDescriptor::TYPE_STRING) return true;
    if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE &&
        ReachesString(field->message_type(), entered)) {
      return true;
    }
  }
  return false;
}

// The fields of a message type that are a string, or a message that can
// hold one. A text cell's box and quad hold none, and a page has thousands
// of cells, so they are not walked.
using TextFields = std::vector<const google::protobuf::FieldDescriptor*>;

const TextFields& TextFieldsOf(const google::protobuf::Descriptor* type) {
  using google::protobuf::FieldDescriptor;
  // One table per thread, so it needs no lock. The contract has a few
  // dozen message types.
  thread_local std::unordered_map<const google::protobuf::Descriptor*,
                                  TextFields>
      known;
  if (const auto it = known.find(type); it != known.end()) return it->second;
  TextFields fields;
  for (int i = 0; i < type->field_count(); ++i) {
    const FieldDescriptor* field = type->field(i);
    if (field->type() == FieldDescriptor::TYPE_STRING) {
      fields.push_back(field);
    } else if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      std::set<const google::protobuf::Descriptor*> entered;
      if (ReachesString(field->message_type(), &entered)) {
        fields.push_back(field);
      }
    }
  }
  return known.emplace(type, std::move(fields)).first->second;
}

}  // namespace

bool IsValidUtf8(std::string_view bytes) {
  for (size_t i = 0; i < bytes.size();) {
    const size_t length = SequenceLength(bytes, i);
    if (length == 0) return false;
    i += length;
  }
  return true;
}

std::string ValidUtf8(std::string_view bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (size_t i = 0; i < bytes.size();) {
    if (const size_t length = SequenceLength(bytes, i); length > 0) {
      out.append(bytes.substr(i, length));
      i += length;
      continue;
    }
    const auto latin1 = static_cast<unsigned char>(bytes[i++]);
    out.push_back(static_cast<char>(0xC0 | (latin1 >> 6)));
    out.push_back(static_cast<char>(0x80 | (latin1 & 0x3F)));
  }
  return out;
}

void MakeStringsValidUtf8(google::protobuf::Message* message) {
  using google::protobuf::FieldDescriptor;
  const google::protobuf::Reflection* reflection = message->GetReflection();
  std::string scratch;
  for (const FieldDescriptor* field : TextFieldsOf(message->GetDescriptor())) {
    if (field->is_repeated()) {
      const int size = reflection->FieldSize(*message, field);
      for (int i = 0; i < size; ++i) {
        if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
          MakeStringsValidUtf8(
              reflection->MutableRepeatedMessage(message, field, i));
          continue;
        }
        const std::string& value = reflection->GetRepeatedStringReference(
            *message, field, i, &scratch);
        if (!IsValidUtf8(value)) {
          reflection->SetRepeatedString(message, field, i, ValidUtf8(value));
        }
      }
      continue;
    }
    // An unset field is left unset: setting it would change what a
    // has_ check on it answers.
    if (!reflection->HasField(*message, field)) continue;
    if (field->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
      MakeStringsValidUtf8(reflection->MutableMessage(message, field));
      continue;
    }
    const std::string& value =
        reflection->GetStringReference(*message, field, &scratch);
    if (!IsValidUtf8(value)) {
      reflection->SetString(message, field, ValidUtf8(value));
    }
  }
}

}  // namespace grpc_qparse
