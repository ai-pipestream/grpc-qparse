# Agent rules for grpc-qparse

One of three interchangeable PDF backend services (grpc-pdfium, grpc-qparse,
grpc-poppler) implementing `PdfBackendService`, package
`ai.protomolt.parse.pdf.v1`. gRParse is the client (single target or a comma
list for consensus mode via `GRPARSE_PDF_BACKEND`).

- **The contract lives in the parser-protos repo**
  (`git.rokkon.com/ai-pipestream/parser-protos`, GitHub mirror of the same
  name). This build downloads the proto files from it at the commit pinned
  in `CMakeLists.txt` (`PDF_PROTOS_COMMIT`, per-file sha256). gRParse
  `backends/` carries identical copies. To change the contract: land the
  same bytes in parser-protos and gRParse `backends/`, then advance the pin
  and hashes here. The contract is additive only; never renumber, retype,
  or remove anything.
- **This project is NOT part of the pipestream-ai platform.** Never put the
  contract in, or take dependencies from, `/work/main/pipestream-ai` or the
  `pipestream-protos` repo. (The contract briefly lived there as a
  `pdf-backend` module; reverted 2026-09-04. Do not repeat that.)
- The engine is commit-pinned and its font resources must be initialised at
  startup (`pdf_resource<PAGE_FONT>::initialise` plus
  `resource_utils::set_resources_dir`) or decode throws `map::at`. Never
  expand a truncated engine commit hash from memory; take it from
  `git rev-parse`.
- The content-addressed handshake (`PdfDocument.sha256`) is served by an
  in-memory LRU byte cache in the server process
  (`src/document_cache.{h,cpp}`), shared by Probe, Parse and Render through
  the one `ResolveDocumentBytes` path in `src/qparse_service_impl.cpp`.
  Bounds: `GRPC_QPARSE_CACHE_MAX_DOCUMENTS` (default 8),
  `GRPC_QPARSE_CACHE_MAX_BYTES` (default 2 GiB). SHA-256 is the
  self-contained `src/sha256.cpp` on purpose: gRPC links its vendored
  BoringSSL statically, and adding a second crypto library risks duplicate
  symbol definitions.
- Default port is 50070 (`GRPC_QPARSE_PORT` overrides), the fleet-registered
  assignment in the workspace table; the backend fleet used to collide with
  gRParse/grPOIc/grpc-libreoffice on 50051-50053.
- `GetServiceInfo` reports the same `backend_name`/`engine_version` strings
  Probe reports, a `build_version` stamped at compile time
  (`-DGRPC_QPARSE_BUILD_VERSION`, git describe in a checkout, `dev`
  fallback), and the family `UiInfo` block (`/ui/qparse`; no web UI yet,
  the field exists so the demo shell tab can appear when one lands).
- The Dockerfile is multi-stage on ubuntu:26.04 (build compiles and runs
  ctest as the image gate; runtime carries the binary plus the engine font
  resources under `/usr/local/share/grpc-qparse/pdf_resources`) and builds
  amd64 only, like the family's other C++ services.
  `.github/workflows/publish.yml` pushes `docker.io/pipestreamai/grpc-qparse:latest`
  on every push to main (plus a `:<version>` tag on manual dispatch) with
  the `DOCKER_USER`/`DOCKER_TOKEN` org secrets, passing the ref name as the
  `GRPC_QPARSE_BUILD_VERSION` build arg so the image reports its tag.
