#!/usr/bin/env bash
# Collects the license texts of everything the image redistributes, one
# directory per component, so the hardened image carries the notices its
# BSD, MIT, IJG, FreeType, zlib-family and OFL dependencies require.
#
#   collect-notices.sh BUILD_DIR SOURCE_DIR OUT_DIR
#
# Runs in the build stage after the build. OUT_DIR gets this repository's
# LICENSE and NOTICE at the top and third_party/<component>/ below them.
# Every listed file must exist: a dependency that moves its license file
# fails the image build here instead of shipping without its notice.
set -euo pipefail

usage() {
  echo "Usage: $0 BUILD_DIR SOURCE_DIR OUT_DIR" >&2
  exit 64
}
[[ $# -eq 3 ]] || usage
build=$1
source=$2
out=$3
grpc=$build/_deps/grpc-src
vendored=$grpc/third_party

# Copies a component's license files into OUT_DIR/third_party/NAME.
copy() {
  local name=$1
  shift
  mkdir -p "$out/third_party/$name"
  for file in "$@"; do
    if [[ ! -f $file ]]; then
      echo "license file for $name not found: $file" >&2
      exit 1
    fi
    cp -L "$file" "$out/third_party/$name/"
  done
}

mkdir -p "$out"
for file in "$source/LICENSE" "$source/NOTICE"; do
  [[ -f $file ]] || { echo "not found: $file" >&2; exit 1; }
  cp "$file" "$out/"
done

copy docling-parse "$build/_deps/qparse_engine-src/LICENSE"
copy pdfium-jbig2 "$source/third_party/pdfium/LICENSE"
copy adobe-cmap-resources "$build/pdf_resources/cmap-resources/LICENSE.md"
copy qpdf "$build/extlib_qpdf/src/extlib_qpdf/LICENSE.txt" \
  "$build/extlib_qpdf/src/extlib_qpdf/NOTICE.md"
copy blend2d "$build/_deps/blend2d-src/LICENSE.md"
copy asmjit "$build/asmjit-src/LICENSE.md"
copy freetype "$build/extlib_freetype/src/extlib_freetype/LICENSE.TXT" \
  "$build/extlib_freetype/src/extlib_freetype/docs/FTL.TXT"
copy libjpeg-turbo "$build/extlib_jpeg/src/extlib_jpeg/LICENSE.md" \
  "$build/extlib_jpeg/src/extlib_jpeg/README.ijg"
copy openjpeg "$build/extlib_openjpeg/src/extlib_openjpeg/LICENSE"
copy lcms2 "$build/extlib_lcms2/src/extlib_lcms2/LICENSE"
copy nlohmann-json "$build/extlib_json/src/extlib_json/LICENSE.MIT"
copy cxxopts "$build/extlib_cxxopts/src/extlib_cxxopts/LICENSE"
copy utfcpp "$build/extlib_utf8/src/extlib_utf8/LICENSE"
copy loguru "$build/_deps/logurugitrepo-src/LICENSE"
copy grpc "$grpc/LICENSE" "$grpc/NOTICE.txt"
copy abseil "$vendored/abseil-cpp/LICENSE"
copy boringssl "$vendored/boringssl-with-bazel/LICENSE"
copy protobuf "$vendored/protobuf/LICENSE"
copy re2 "$vendored/re2/LICENSE"
copy address_sorting "$vendored/address_sorting/LICENSE"
copy xxhash "$vendored/xxhash/LICENSE"
copy utf8_range "$vendored/utf8_range/LICENSE"
copy c-ares "$vendored/cares/cares/LICENSE.md"
copy zlib "$vendored/zlib/LICENSE"
copy liberation-fonts "$build/pdf_resources/fonts/fallback/LICENSE" \
  "$build/pdf_resources/fonts/fallback/AUTHORS"
# The shared libraries staged next to the binary come from the build
# stage's Debian packages.
copy debian-zlib1g /usr/share/doc/zlib1g/copyright
copy debian-libstdc++6 /usr/share/doc/libstdc++6/copyright
copy debian-libgcc-s1 /usr/share/doc/libgcc-s1/copyright

echo "== notices collected into $out"
find "$out" -type f | sort
