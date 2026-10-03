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
- Page geometry follows the contract frame: every cell, quad, shape, image,
  link and widget is in unrotated PDF user space (absolute, not shifted to
  the CropBox origin), `PageInfo.rotation_degrees` is the page's own or
  inherited `/Rotate`, and `media_box`/`crop_box` are the unrotated boxes.
  gRParse's `PageFrame` maps that frame onto the rendered page. The engine
  would rotate items into display orientation and drop the angle, so pages
  are decoded through the service's own qpdf handle (`DocumentPages` in
  `src/qparse_service_impl.cpp`) with `/Rotate` held at 0 and the inherited
  boxes pinned on the page; Render hands the rotation to the rasterizer
  through the size instruction. Do not go back to the engine's document
  `decode_page`. The page inventory comes from the page dictionaries, so a
  Parse header never decodes content, and a call decodes only the pages in
  its range (checking for cancellation between pages).
  `test/fixtures/frames.pdf` (`make_frames_pdf.py`) pins all of this.
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
- The Dockerfile is multi-stage: the build stage (Debian trixie
  toolchain) compiles and runs ctest as the image gate; the runtime is the
  hardened `dhi.io/debian-base:trixie-debian13` base (glibc only, no
  package manager, no ldconfig, uid 65532) carrying the binary, the engine
  font resources under `/usr/local/share/grpc-qparse/pdf_resources` (with
  the pinned Liberation fallback faces in `fonts/fallback`, which the
  rasterizer needs for every non-embedded font because the base has no
  system fonts; the render test checks for real glyph ink), and
  the staged shared-library closure (`scripts/stage-runtime-libs.sh`)
  under `/usr/local/lib` on `LD_LIBRARY_PATH`, plus `LICENSE`, `NOTICE`
  and each redistributed component's license text under
  `/usr/local/share/doc/grpc-qparse` (`scripts/collect-notices.sh`, which
  fails the build when a listed file is missing; a new dependency needs a
  line there and in `NOTICE`). The build stage must stay on
  a glibc no newer than the runtime base's (2.41); a build on ubuntu 26.04
  produces a binary the base cannot load. `GRPC_QPARSE_RUNTIME_IMAGE`
  swaps the base. Published as a linux/amd64 + linux/arm64 manifest list,
  each leg built and smoke-tested natively on its own architecture; the
  arm64 leg runs on GitHub's hosted `ubuntu-24.04-arm` runner. `scripts/smoke-test.sh IMAGE`
  is the boot gate (closure, boot to listening under the hardened flags,
  uid); `ci.yml` runs the image build then the smoke test, and
  `.github/workflows/publish.yml` builds each platform leg, pushes it by
  digest only, boot-proofs the pushed digest, and only then assembles the
  passing digests into
  `docker.io/pipestreamai/grpc-qparse:latest` on every push to main (plus
  a `:<version>` tag on manual dispatch) with the
  `DOCKER_USER`/`DOCKER_TOKEN` org secrets, passing the dispatch version,
  or the commit sha on a push, as the `GRPC_QPARSE_BUILD_VERSION` build arg
  so the image reports which build it is. `ci.yml` and `publish.yml` both
  run with a read-only `contents` token.
