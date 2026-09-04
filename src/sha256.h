#pragma once

#include <string>
#include <string_view>

namespace grpc_qparse {

// Lowercase hex SHA-256 of `data`, the form PdfDocument.sha256 carries.
// Self-contained (FIPS 180-4) rather than a crypto library: the server links
// gRPC's vendored BoringSSL statically, and linking a second crypto library
// into the same binary is how a process ends up with two incompatible
// definitions of the same symbol.
std::string Sha256Hex(std::string_view data);

}  // namespace grpc_qparse
