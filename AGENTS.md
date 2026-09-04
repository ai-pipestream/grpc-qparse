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
