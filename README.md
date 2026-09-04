# grpc-qparse

A gRPC PDF backend service over the MIT-licensed qpdf-based cell parser,
implementing the fleet's common `PdfBackendService` contract
(`ai.protomolt.parse.pdf.v1`, from the parser-protos commit this
build pins). The wrapper is Apache-2.0; the engine and its dependency set
(qpdf, blend2d, freetype, openjpeg, lcms2, libjpeg) are all permissive.

What this backend is for: reading-order text cells (its sanitizers merge
raw chars into model-ready cells with direction, space width and rendering
mode), vector shapes, embedded font programs, plus the tier 0 floor: typed
load status, page inventory, and blend2d page rasters (RGBA8). Placed
images, hyperlinks, form-field widgets, the outline, and the XMP packet
ride along. Families the engine's public surface does not expose
(annotations as typed data, encryption details, attachments, signatures,
JavaScript, the structure tree, thumbnails, and its internal resource
dictionaries) are reported unsupported in `Probe`, each with a reason.

The engine is safe to use concurrently through independent per-request
decoder instances, so the service is plain thread-per-request; there is no
worker-process pool here.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The build pins the engine by commit (FetchContent) and compiles its
dependency set from source the way the engine's own tree does. The
contract protos come from the pinned parser-protos commit
(sha256-verified). `-DPDF_PROTO_LOCAL_DIR=/path/to/gRParse/backends`
switches to a local directory of proto files for contract development.

## Run

```bash
GRPC_QPARSE_PORT=50070 GRPC_QPARSE_RESOURCES=./build/pdf_resources ./build/grpc_qparse
```

50070 is the fleet-registered default port (the fleet table lives in the
workspace `AGENTS.md`); `GRPC_QPARSE_PORT` overrides it.

`GRPC_QPARSE_RESOURCES` points at the engine's font resource directory
(staged into the build tree at configure time). Health and server
reflection are enabled; `Probe`, `Parse`, and `Render` are the service
surface.

## Content-addressed documents

The contract lets a client upload a document once and address it by hash
afterwards: send `data` together with `sha256` (the lowercase hex SHA-256
of `data`) on the first call, then call with `sha256` alone. The service
keeps the bytes in a small in-memory LRU cache in the single server
process and answers a lookup it cannot satisfy with the typed
`LOAD_STATUS_BYTES_REQUIRED` verdict (in `ProbeResponse.capabilities`, the
`ParseHeader` capabilities, or the `RenderResponse` head), which the
client answers by retrying exactly once with the bytes. Bytes that do not
hash to the given `sha256` get `LOAD_STATUS_HASH_MISMATCH` on the same
surfaces; `data` empty with no `sha256` is `INVALID_ARGUMENT`.

Cache bounds come from the environment:

| Variable | Default | Meaning |
|---|---|---|
| `GRPC_QPARSE_CACHE_MAX_DOCUMENTS` | 8 | Documents kept at most; 0 disables caching. |
| `GRPC_QPARSE_CACHE_MAX_BYTES` | 2147483648 (2 GiB) | Total cached bytes ceiling; a document larger than this is never cached. |

Eviction is least-recently-used. Cache lifetime is the process lifetime;
the contract promises only the verdicts, never retention.
