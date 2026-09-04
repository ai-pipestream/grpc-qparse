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
GRPC_QPARSE_PORT=50052 GRPC_QPARSE_RESOURCES=./build/pdf_resources ./build/grpc_qparse
```

`GRPC_QPARSE_RESOURCES` points at the engine's font resource directory
(staged into the build tree at configure time). Health and server
reflection are enabled; `Probe`, `Parse`, and `Render` are the service
surface.
