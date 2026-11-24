#ifndef GRPC_QPARSE_VALID_UTF8_H_
#define GRPC_QPARSE_VALID_UTF8_H_

#include <string>
#include <string_view>

#include <google/protobuf/message.h>

// The contract's string fields are proto3 strings, and protobuf refuses a
// message that holds one which is not UTF-8: a single font name in a
// legacy encoding would cost the client the rest of the stream. A PDF name
// is bytes (a /BaseFont in GBK, a choice value, an appearance state, an
// info key), and so is what the engine passes on from one, so every
// message is made valid here on its way out, whichever reader filled it.

namespace grpc_qparse {

// Bytes as valid UTF-8. Well-formed UTF-8 passes through unchanged; each
// byte of an ill-formed sequence is read as Latin-1, since PDF names and
// legacy byte strings are most often in a single-byte encoding. The same
// rule as grpc-pdfium's and grpc-poppler's, so the three backends report
// the same name for the same bytes.
std::string ValidUtf8(std::string_view bytes);

// Whether bytes are well-formed UTF-8 already.
bool IsValidUtf8(std::string_view bytes);

// Applies ValidUtf8 to every string field of the message and of the
// messages inside it. Bytes fields are left alone.
void MakeStringsValidUtf8(google::protobuf::Message* message);

}  // namespace grpc_qparse

#endif  // GRPC_QPARSE_VALID_UTF8_H_
