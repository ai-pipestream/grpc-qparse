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
widgets, the outline, and document metadata (the info dictionary, the
PDF version and the XMP packet) ride along. Families the engine's
public surface does not expose (annotations as typed data, encryption
details, attachments, signatures, JavaScript, the structure tree,
thumbnails, and its internal resource dictionaries) are reported
unsupported in `Probe`, each with a reason.

Every geometry is in the contract's frame: unrotated PDF user space
shifted so the CropBox's lower-left corner is (0, 0). `PageInfo` says so
with `page_space = PAGE_SPACE_CROP_BOX` and carries the page's true
`/Rotate` and its MediaBox and CropBox as stored.
A font id names one font for the whole stream: a cell's `font_id` is the
id of the `EmbeddedFont` that carries its program, and two subsets that
share a name but not a program get two ids. `PDF_FAMILY_FONTS` sends the
font table; programs are decoded and sent, once each, only when
`PDF_FAMILY_EMBEDDED_FONTS` is requested. A `FontRef` is marked
`embedded` whenever its font dictionary carries a program (`/FontFile`,
`/FontFile2` or `/FontFile3`), whether or not the call decodes it.

A document that does not open gets the typed verdict qpdf's error code
gives: `LOAD_STATUS_PASSWORD_REQUIRED` for an encrypted file opened
without a password, `LOAD_STATUS_PASSWORD_INCORRECT` when the password
given does not open it, `LOAD_STATUS_CORRUPT` for damage qpdf cannot
recover from, and `LOAD_STATUS_ENGINE_ERROR` for anything else.

The engine is safe to use concurrently through independent per-request
decoder instances, so the service is plain thread-per-request; there is no
worker-process pool here. A cap on concurrent `Parse` and `Render` calls
bounds memory instead: one `Render` at the full pixel budget holds about
2 GiB (the canvas, its copy, the message and gRPC's serialized buffer). A
call over the cap waits for a slot no longer than its client does, nor
than the queue timeout, and then fails `RESOURCE_EXHAUSTED`. `Probe` and
`GetServiceInfo` are not capped.

| Variable | Default | Meaning |
|---|---|---|
| `GRPC_QPARSE_MAX_CONCURRENT_CALLS` | half the cores, at least 2 | `Parse` and `Render` calls running at once; unset, 0 or unparseable keeps the default. Lower it when the container has less than about 2 GiB per call. |
| `GRPC_QPARSE_QUEUE_TIMEOUT_S` | 300 | Seconds a call waits for a slot; 0 leaves only the client's deadline. A value that is not whole seconds up to 604800 (a week) keeps the default. |

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

## Page ranges

A set `PageRange` must have `end` greater than `begin`, the contract's one
rule for it; anything else fails `Parse` and `Render` with
`INVALID_ARGUMENT` before a message streams. Every other range is valid:
an `end` past the document stops at its last page, and a range that starts
past it selects no page, however large its `begin`.

## Render options

`Render` honors the request's `background` and `omit_annotations`. The
background is painted under every page item; each component is clamped to
[0, 1], and one that is not a finite number is `INVALID_ARGUMENT`. Unset,
the page sits on opaque white. Rasters carry no transparency, so a
background with alpha below 1 is laid over that white. With
`omit_annotations` set, the page's annotations (markup and form widgets
alike) are left out of the raster and only the content stream is drawn;
`Parse` is unaffected.

## Render bounds

`Render` takes a positive finite `dpi`; anything else (zero, negative,
NaN, infinity) is `INVALID_ARGUMENT`. Before it renders a page it sizes
every page in the range, and a page whose raster would be wider or taller
than 65535 pixels (the rasterizer's limit) or larger than the pixel budget
fails the whole call with `RESOURCE_EXHAUSTED`, before any raster streams.
A page the engine cannot decode or the rasterizer fails on ends the call
with `INTERNAL` at that page: the status message names the page index and
the reason (`Render failed on page 3: ...`), and the same line is logged
at ERROR. Rasters stream in page order, so the pages before it have
arrived and none follows; an OK `Render` stream always carries every page
of its range. Over `ParseStream` the `RenderPages` request ends
`REQUEST_OUTCOME_ENGINE_ERROR` with that message as its detail, and the
stream and its other requests carry on. `Parse` is different on purpose:
it has a typed per-page warning in its trailer, so it leaves the page's
chunk out, names the page there and finishes OK. The budget comes from
the environment:

| Variable | Default | Meaning |
|---|---|---|
| `GRPC_QPARSE_RENDER_MAX_PIXELS` | 134217728 (2^27) | Pixels one page's raster may have; also the ceiling, since a 2^27-pixel RGBA raster is the most one 520 MiB message carries. Also the most pixels an image XObject (or its soft mask) may declare. |
| `GRPC_QPARSE_MAX_DECODED_STREAM_BYTES` | 536870912 (512 MiB) | Bytes one Flate, RunLength or LZW stream may decode to, on every RPC, and the memory jpeglib may use for a DCT stream qpdf decodes; qpdf holds it process-wide. |

Image samples are bounded before they are decoded. An image XObject a
page reaches (through its forms, patterns and Type3 fonts too) that
declares more pixels than the budget, or whose `/SMask` or `/Mask` does,
is never decoded, and a page that draws it is not rendered without it:
the call ends `RESOURCE_EXHAUSTED` at that page, naming the page and the
image (`Render failed on page 1: it draws image /Im0, ...`), after the
pages before it. A page that only shares a resource dictionary with such
an image and does not draw it renders as usual. `/Width` and
`/Height` count whether they are integers or reals, as the engine reads
both. A stream that inflates past the decoded stream limit, whatever size
it declares, is cut off there and treated as undecodable. qpdf's own
`/LZWDecode` has no such limit, so the service registers a replacement
that has; qpdf's other decoders (ASCIIHex, ASCII85, Crypt) never give more
than four bytes per input byte. `Parse` never decodes image
samples, so it still reports every image's placement and size.

## Damaged documents

qpdf repairs what it can and carries on, and the engine draws what is
left. Neither is passed off as a whole document:

- What qpdf says while it opens the document (a cross-reference table it
  had to rebuild, an object it could not read) rides in the `Parse`
  trailer as document-level `ParseWarning`s, each message starting
  `qpdf: `. What it says while a page is decoded rides there with that
  page's index. A page or a load lists at most 16 different warnings and
  counts the rest in one more.
- qpdf drops a page-tree kid it cannot read without a warning, so the
  service compares the page tree's `/Count` with the pages found: a
  difference is a `PDF_FAMILY_PAGE_INVENTORY` warning, and a document
  that declares pages, or needed a repair, and has no page left is
  `LOAD_STATUS_CORRUPT`, from `Probe`, `Parse` and `Render` alike.
- A page whose content stream does not decode (a damaged or mislabelled
  filter, or a stream over `GRPC_QPARSE_MAX_DECODED_STREAM_BYTES`) would
  come out of the engine as a blank page. `Parse` still sends the page's
  chunk, with what the page has besides its content, and warns on that
  page (`content stream does not decode, ...`); `Render` ends `INTERNAL`
  at it. The service asks qpdf to decode the page's content streams
  before the engine does, which costs one more decode of them per page.

Not yet said: an image whose samples do not decode is still left out of a
`Render` raster without a word, because `PageRaster` has no field to say
it in.

## Document metadata

`DocMeta` comes from the trailer's info dictionary (the standard text
keys as UTF-8 whatever their encoding in the file, both dates and
`/Trapped` as stored, every other key as a `CustomInfoEntry`), the PDF
version from the header or a later catalog `/Version`, `linearized`,
`tagged` from the catalog's `/MarkInfo /Marked`, and the XMP packet. An
info value that is not text, a name, a number or a boolean is not carried
and gets a `PDF_FAMILY_DOC_METADATA` warning naming its key.
