cmake_minimum_required(VERSION 3.24)
project(grpc_qparse CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

add_library(grpc_qparse_flags INTERFACE)
target_compile_options(grpc_qparse_flags INTERFACE -Wall -Wextra)

# ---------------------------------------------------------------------------
# gRPC + protobuf, the fleet's C++ pattern (same tag as gRParse).
# ---------------------------------------------------------------------------
set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(grpc
  GIT_REPOSITORY https://github.com/grpc/grpc.git
  GIT_TAG v1.83.1
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(grpc)

# ---------------------------------------------------------------------------
# The PdfBackendService contract (package ai.protomolt.parse.pdf.v1),
# fetched from the parser-protos repo at a pinned commit; gRParse backends/
# holds the same bytes. -DPDF_PROTO_LOCAL_DIR=<directory holding the .proto
# files> overrides for contract development only.
# The download comes from the GitHub mirror's raw URL, not git.rokkon.com:
# GitHub-hosted runners cannot resolve the Forgejo host, and the per-file
# sha256 pins below verify the bytes either way.
# The pins here and the engine and font pins below are plain variables, not
# cache entries: a cache entry keeps the value of the first configure, so a
# build tree reused across a pin bump (the Dockerfile's build cache mount, a
# developer's build directory) would quietly keep building the old commit.
# Bump them here.
# ---------------------------------------------------------------------------
set(PDF_PROTOS_COMMIT "ba226130c49683a8e629a6d3dc0f58c83c51c628")
set(PDF_PROTOS_BASE_URL
    "https://raw.githubusercontent.com/ai-pipestream/parser-protos/${PDF_PROTOS_COMMIT}/proto/ai/protomolt/parse/pdf/v1")
set(PDF_PROTO_TYPES_SHA256
    "27c9d9e4f822650344e609a1ca4ca79e7d7f2d867c0517f72bfa6982af8e8edd")
set(PDF_PROTO_SERVICE_SHA256
    "f0d47132b48af9cf09007afac32b26ef8f8e521b16f7bc105780d6afef5e4e8a")
set(PDF_PROTO_LOCAL_DIR "" CACHE PATH "directory holding the contract protos, instead of the pinned download")

set(STAGED_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(PDF_PROTO_REL "ai/protomolt/parse/pdf/v1")

if(PDF_PROTO_LOCAL_DIR)
  set(PDF_PROTO_SRC "${PDF_PROTO_LOCAL_DIR}")
else()
  set(PDF_PROTO_SRC "${CMAKE_CURRENT_BINARY_DIR}/protos-export/${PDF_PROTOS_COMMIT}")
  foreach(PROTO_SPEC IN ITEMS
      "pdf_backend_types.proto;${PDF_PROTO_TYPES_SHA256}"
      "pdf_backend_service.proto;${PDF_PROTO_SERVICE_SHA256}")
    list(GET PROTO_SPEC 0 PROTO_NAME)
    list(GET PROTO_SPEC 1 PROTO_SHA)
    if(NOT EXISTS "${PDF_PROTO_SRC}/${PROTO_NAME}")
      file(DOWNLOAD "${PDF_PROTOS_BASE_URL}/${PROTO_NAME}"
        "${PDF_PROTO_SRC}/${PROTO_NAME}"
        EXPECTED_HASH SHA256=${PROTO_SHA}
        STATUS PROTO_DL_STATUS)
      list(GET PROTO_DL_STATUS 0 PROTO_DL_CODE)
      if(NOT PROTO_DL_CODE EQUAL 0)
        file(REMOVE "${PDF_PROTO_SRC}/${PROTO_NAME}")
        message(FATAL_ERROR "contract proto download failed: ${PROTO_DL_STATUS}")
      endif()
    endif()
  endforeach()
endif()

file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}")
configure_file("${PDF_PROTO_SRC}/pdf_backend_types.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_types.proto" COPYONLY)
configure_file("${PDF_PROTO_SRC}/pdf_backend_service.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_service.proto" COPYONLY)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

set(PDF_BACKEND_PROTOS
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_types.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_service.proto")
set(PDF_BACKEND_GENERATED
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.pb.h"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.grpc.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.grpc.pb.h"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.pb.h"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.grpc.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.grpc.pb.h")

add_custom_command(
  OUTPUT ${PDF_BACKEND_GENERATED}
  COMMAND $<TARGET_FILE:protobuf::protoc>
    --proto_path=${STAGED_PROTO_DIR}
    --cpp_out=${GENERATED_DIR}
    --grpc_out=${GENERATED_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
    ${PDF_BACKEND_PROTOS}
  DEPENDS ${PDF_BACKEND_PROTOS} protobuf::protoc grpc_cpp_plugin
  COMMENT "Generating C++ stubs for the PdfBackendService contract")

add_library(pdf_backend_proto STATIC ${PDF_BACKEND_GENERATED})
target_include_directories(pdf_backend_proto SYSTEM PUBLIC "${GENERATED_DIR}")
target_link_libraries(pdf_backend_proto PUBLIC grpc++ grpc++_reflection libprotobuf)

# ---------------------------------------------------------------------------
# The MIT engine (commit-pinned), built the way its own tree builds: its
# extlib modules compile the dependency set from source (qpdf, jpeg,
# openjpeg, lcms2, blend2d, freetype, and friends) and the engine itself is
# a header library under src/. Only the library surface is used here; the
# upstream app and python-binding targets are never configured.
# ---------------------------------------------------------------------------
# The engine commit; a plain variable like the contract pins above.
set(QPARSE_ENGINE_TAG "824063d392142ffa5d0097dfc13faf963a9be113")
FetchContent_Declare(qparse_engine
  GIT_REPOSITORY https://github.com/docling-project/docling-parse.git
  GIT_TAG ${QPARSE_ENGINE_TAG})
FetchContent_Populate(qparse_engine)

set(TOPLEVEL_PREFIX_PATH ${qparse_engine_SOURCE_DIR})
# The engine's own root build adds src/ globally; its jbig2 extlib target
# compiles engine sources that include through that path.
include_directories("${TOPLEVEL_PREFIX_PATH}/src")
set(EXTERNALS_PREFIX_PATH "${CMAKE_CURRENT_BINARY_DIR}/engine-externals" CACHE INTERNAL "")
set(RESOURCES_PREFIX_PATH "${TOPLEVEL_PREFIX_PATH}/resources" CACHE INTERNAL "")
if(NOT EXISTS ${EXTERNALS_PREFIX_PATH})
  file(MAKE_DIRECTORY ${EXTERNALS_PREFIX_PATH})
  file(MAKE_DIRECTORY ${EXTERNALS_PREFIX_PATH}/bin)
  file(MAKE_DIRECTORY ${EXTERNALS_PREFIX_PATH}/lib)
  file(MAKE_DIRECTORY ${EXTERNALS_PREFIX_PATH}/include)
  file(MAKE_DIRECTORY ${EXTERNALS_PREFIX_PATH}/resources)
endif()
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_cxxopts.cmake)
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_loguru.cmake)
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_json.cmake)
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_utf8.git.cmake)
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_jpeg.cmake)
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_openjpeg.cmake)
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_lcms2.cmake)
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_qpdf_v12.cmake)
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_blend2d.cmake)
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_freetype.cmake)
include(${TOPLEVEL_PREFIX_PATH}/cmake/extlib_pdfium_jbig2.cmake)
set(ENGINE_DEPENDENCIES qpdf jpeg openjpeg lcms2 utf8 json loguru cxxopts blend2d freetype pdfium_jbig2)
include(${TOPLEVEL_PREFIX_PATH}/cmake/os_opts.cmake)
list(APPEND ENGINE_DEPENDENCIES ${OS_DEPENDENCIES})

# The engine reads font metric resources at runtime; stage them next to the
# binary so the service can point resource_utils at them.
set(QPARSE_RESOURCES_DIR "${CMAKE_CURRENT_BINARY_DIR}/pdf_resources")
file(COPY "${TOPLEVEL_PREFIX_PATH}/docling_parse/pdf_resources/"
  DESTINATION "${QPARSE_RESOURCES_DIR}")

# Fallback faces for the rasterizer. The engine draws a font that is not
# embedded (the standard 14 among them) with a face it finds by name under
# <resources>/fonts/fallback or the system font directories, and the
# runtime image has no system fonts, so without these every such glyph came
# out as a bare outline box. Liberation Sans, Serif and Mono are
# metric-compatible with Helvetica, Times and Courier; SIL OFL 1.1, whose
# LICENSE ships beside the faces. Pinned by version and sha256, and the
# archive is re-verified on every configure.
set(QPARSE_FALLBACK_FONTS_VERSION "2.1.5")
set(QPARSE_FALLBACK_FONTS_URL
    "https://github.com/liberationfonts/liberation-fonts/files/7261482/liberation-fonts-ttf-${QPARSE_FALLBACK_FONTS_VERSION}.tar.gz")
set(QPARSE_FALLBACK_FONTS_SHA256
    "7191c669bf38899f73a2094ed00f7b800553364f90e2637010a69c0e268f25d0")
set(QPARSE_FONTS_EXPORT "${CMAKE_CURRENT_BINARY_DIR}/fonts-export")
set(QPARSE_FALLBACK_FONTS_ARCHIVE
    "${QPARSE_FONTS_EXPORT}/liberation-fonts-ttf-${QPARSE_FALLBACK_FONTS_VERSION}.tar.gz")
if(EXISTS "${QPARSE_FALLBACK_FONTS_ARCHIVE}")
  file(SHA256 "${QPARSE_FALLBACK_FONTS_ARCHIVE}" FONTS_ARCHIVE_SHA256)
  if(NOT FONTS_ARCHIVE_SHA256 STREQUAL QPARSE_FALLBACK_FONTS_SHA256)
    file(REMOVE "${QPARSE_FALLBACK_FONTS_ARCHIVE}")
  endif()
endif()
if(NOT EXISTS "${QPARSE_FALLBACK_FONTS_ARCHIVE}")
  file(DOWNLOAD "${QPARSE_FALLBACK_FONTS_URL}" "${QPARSE_FALLBACK_FONTS_ARCHIVE}"
    EXPECTED_HASH SHA256=${QPARSE_FALLBACK_FONTS_SHA256}
    STATUS FONTS_DL_STATUS)
  list(GET FONTS_DL_STATUS 0 FONTS_DL_CODE)
  if(NOT FONTS_DL_CODE EQUAL 0)
    file(REMOVE "${QPARSE_FALLBACK_FONTS_ARCHIVE}")
    message(FATAL_ERROR "fallback font download failed: ${FONTS_DL_STATUS}")
  endif()
endif()
file(ARCHIVE_EXTRACT INPUT "${QPARSE_FALLBACK_FONTS_ARCHIVE}"
  DESTINATION "${QPARSE_FONTS_EXPORT}")
set(QPARSE_FALLBACK_FONTS_DIR
    "${QPARSE_FONTS_EXPORT}/liberation-fonts-ttf-${QPARSE_FALLBACK_FONTS_VERSION}")
file(COPY "${QPARSE_FALLBACK_FONTS_DIR}/"
  DESTINATION "${QPARSE_RESOURCES_DIR}/fonts/fallback"
  FILES_MATCHING PATTERN "*.ttf" PATTERN "LICENSE" PATTERN "AUTHORS")

# ---------------------------------------------------------------------------
# The service binary.
# ---------------------------------------------------------------------------
# The build version GetServiceInfo reports: the image tag when built in
# Docker (passed as -DGRPC_QPARSE_BUILD_VERSION), git describe in a checkout,
# "dev" as the fallback.
set(GRPC_QPARSE_BUILD_VERSION "" CACHE STRING
    "build version stamped into GetServiceInfo, for example the image tag")
if(NOT GRPC_QPARSE_BUILD_VERSION)
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE GRPC_QPARSE_GIT_DESCRIBE
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
  endif()
  if(GRPC_QPARSE_GIT_DESCRIBE)
    set(GRPC_QPARSE_BUILD_VERSION "${GRPC_QPARSE_GIT_DESCRIBE}")
  else()
    set(GRPC_QPARSE_BUILD_VERSION "dev")
  endif()
endif()

add_executable(grpc_qparse
  src/main.cpp
  src/qparse_service_impl.cpp
  src/document_cache.cpp
  src/sha256.cpp)
target_include_directories(grpc_qparse PRIVATE src)
target_include_directories(grpc_qparse SYSTEM PRIVATE "${TOPLEVEL_PREFIX_PATH}/src")
target_compile_definitions(grpc_qparse PRIVATE
  ROOT_PATH="${TOPLEVEL_PREFIX_PATH}" LOGURU_WITH_STREAMS=1
  GRPC_QPARSE_BUILD_VERSION="${GRPC_QPARSE_BUILD_VERSION}")
target_link_libraries(grpc_qparse PRIVATE pdf_backend_proto ${ENGINE_DEPENDENCIES} grpc_qparse_flags)

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(qparse_contract_test
  test/contract_roundtrip_test.cpp
  src/qparse_service_impl.cpp
  src/document_cache.cpp
  src/sha256.cpp)
target_include_directories(qparse_contract_test PRIVATE src)
target_include_directories(qparse_contract_test SYSTEM PRIVATE "${TOPLEVEL_PREFIX_PATH}/src")
target_compile_definitions(qparse_contract_test PRIVATE
  ROOT_PATH="${TOPLEVEL_PREFIX_PATH}" LOGURU_WITH_STREAMS=1
  GRPC_QPARSE_BUILD_VERSION="${GRPC_QPARSE_BUILD_VERSION}")
target_link_libraries(qparse_contract_test PRIVATE pdf_backend_proto ${ENGINE_DEPENDENCIES} grpc_qparse_flags)
add_test(NAME qparse_contract COMMAND qparse_contract_test
  "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/hello.pdf"
  "${QPARSE_RESOURCES_DIR}")
