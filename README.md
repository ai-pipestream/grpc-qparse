# grpc-qparse

A gRPC PDF backend service over the MIT-licensed qpdf-based cell parser,
implementing the fleet's common `PdfBackendService` contract
(`ai.protomolt.parse.pdf.v1`, from the parser-protos commit this
build pins). The wrapper is Apache-2.0; the engine and its dependency set
(qpdf, blend2d, freetype, openjpeg, lcms2, libjpeg) are all permissive.
`NOTICE` lists every component the image redistributes with its license,
and the image carries their license texts under
`/usr/local/share/doc/grpc-qparse`.

What this backend is for: reading-order text cells (its sanitizers merge
raw chars into model-ready cells with direction, space width and rendering
mode), vector shapes, embedded font programs, plus the tier 0 floor: typed
load status, page inventory, and blend2d page rasters (RGBA8, BGRA8, RGB8,
BGR8 or GRAY8, as requested). Placed images, hyperlinks, form-field
widgets, the outline, and the XMP packet ride along. Families the engine's
public surface does not expose (annotations as typed data, encryption
details, attachments, signatures, JavaScript, the structure tree,
thumbnails, and its internal resource dictionaries) are reported
unsupported in `Probe`, each with a reason.

Every geometry is in unrotated PDF user space, the contract's frame, with
`PageInfo` carrying the page's true `/Rotate` and its MediaBox and CropBox.
A font id names one font for the whole stream: a cell's `font_id` is the
id of the `EmbeddedFont` that carries its program, and two subsets that
share a name but not a program get two ids. `PDF_FAMILY_FONTS` sends the
font table; programs are decoded and sent, once each, only when
`PDF_FAMILY_EMBEDDED_FONTS` is requested.

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
(staged into the build tree at configure time). It also holds the
rasterizer's fallback faces under `fonts/fallback`: Liberation Sans, Serif
and Mono 2.1.5 (SIL OFL 1.1, license beside them), downloaded at configure
time from the pinned release and sha256-verified. Text in a font the PDF
does not embed, the standard 14 included, is drawn with them; without
them it would rasterize as outline boxes, since the runtime image has no
system fonts. Health and server reflection are enabled; `Probe`, `Parse`,
`Render`, and `GetServiceInfo` are the service surface.

## Docker

```bash
docker build -t grpc-qparse .
docker run --rm --read-only -p 50070:50070 grpc-qparse
scripts/smoke-test.sh grpc-qparse     # boot-proof a built image
```

The build stage (a Debian trixie toolchain) compiles the service and runs
the contract test as the image gate. The runtime stage is the hardened
`dhi.io/debian-base:trixie-debian13` base: glibc and nothing else, no
package manager, no ldconfig, and the service runs as uid 65532 out of the
box, so no `--user` flag is needed. It carries the binary, the engine's
font resources (wired up through `GRPC_QPARSE_RESOURCES`, unchanged at
`/usr/local/share/grpc-qparse/pdf_resources`), and the shared libraries
the binary needs beyond glibc (libstdc++, libgcc_s, libz), staged from the
build stage into `/usr/local/lib` and found through `LD_LIBRARY_PATH`;
`scripts/stage-runtime-libs.sh` copies that closure at build time and
fails the build if anything would resolve from outside it. The base is
swappable with `--build-arg GRPC_QPARSE_RUNTIME_IMAGE=<image>` for any
image whose glibc is 2.41 or newer. Nothing is written at runtime, so the
container runs read-only without a tmpfs.

`scripts/smoke-test.sh IMAGE` is the boot gate CI and the publish workflow
run before any push: the library closure resolves inside the image (the
dynamic loader reports it, since the base has no `ldd`), the server
reaches its "listening on" line under `--read-only --cap-drop ALL` (which
also proves the engine found its font resources), every process runs as
uid 65532, and the license texts are in the image
(`scripts/collect-notices.sh` gathers them in the build stage and fails
the build if one has moved). Published as a linux/amd64 + linux/arm64
manifest list, each leg built and smoke-tested natively on its own
architecture (the arm64 leg runs on GitHub's hosted arm64 runner). The
publish workflow pushes
`docker.io/pipestreamai/grpc-qparse:latest` on every push to main.

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

## Render bounds

`Render` takes a positive finite `dpi`; anything else (zero, negative,
NaN, infinity) is `INVALID_ARGUMENT`. Before it renders a page it sizes
every page in the range, and a page whose raster would be wider or taller
than 65535 pixels (the rasterizer's limit) or larger than the pixel budget
fails the whole call with `RESOURCE_EXHAUSTED`, before any raster streams.
A page the engine or the rasterizer fails on is skipped and the rest of
the range still renders. The budget comes from the environment:

| Variable | Default | Meaning |
|---|---|---|
| `GRPC_QPARSE_RENDER_MAX_PIXELS` | 134217728 (2^27) | Pixels one page's raster may have; also the ceiling, since a 2^27-pixel RGBA raster is the most one 520 MiB message carries. |
