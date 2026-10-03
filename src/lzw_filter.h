#pragma once

#include <cstdint>

namespace grpc_qparse {

// Replaces qpdf's /LZWDecode filter, for every QPDF in the process, with
// one that stops once a stream has decoded to limit_bytes. qpdf's own LZW
// decoder has no limit: a clear code resets its table, so a stream can
// repeat its longest runs for as long as it likes, about 1300 decoded
// bytes per encoded byte, and a few megabytes of LZW content or image data
// decode to gigabytes. A stream cut off by the limit throws inside qpdf's
// pipeline, the same way Pl_Flate does at its limit, and the stream is
// treated as undecodable. A /Predictor in /DecodeParms is applied after the
// limit, by qpdf's own predictor code.
void InstallLimitedLzwDecode(uint64_t limit_bytes);

}  // namespace grpc_qparse
