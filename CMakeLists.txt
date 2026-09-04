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
# The PdfBackendService contract, from the pinned pipestream-protos release.
# -DPDF_PROTO_LOCAL_DIR=<pipestream-protos checkout> overrides for contract
# development only.
# ---------------------------------------------------------------------------
set(PDF_PROTOS_VERSION "0.15.0" CACHE STRING "pipestream-protos release the contract comes from")
set(PDF_PROTOS_SHA256 "558974e94c148b351f7dce74ac1189bd41e0433d5fa104cffa21c82c62c6db9b"
    CACHE STRING "sha256 of the release proto tarball")
set(PDF_PROTOS_URL
    "https://git.rokkon.com/api/packages/ai-pipestream/generic/pipestream-protos/${PDF_PROTOS_VERSION}/pipestream-protos-${PDF_PROTOS_VERSION}.tgz"
    CACHE STRING "URL of the release proto tarball")
set(PDF_PROTO_LOCAL_DIR "" CACHE PATH "local pipestream-protos checkout to use instead of the release tarball")

set(STAGED_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(PDF_PROTO_REL "ai/pipestream/parse/pdf/v1")

if(PDF_PROTO_LOCAL_DIR)
  set(PDF_PROTO_SRC "${PDF_PROTO_LOCAL_DIR}/pdf-backend/proto/${PDF_PROTO_REL}")
else()
  set(PROTOS_TGZ "${CMAKE_CURRENT_BINARY_DIR}/pipestream-protos-${PDF_PROTOS_VERSION}.tgz")
  if(NOT EXISTS "${PROTOS_TGZ}")
    file(DOWNLOAD "${PDF_PROTOS_URL}" "${PROTOS_TGZ}"
      EXPECTED_HASH SHA256=${PDF_PROTOS_SHA256}
      STATUS PROTOS_DL_STATUS)
    list(GET PROTOS_DL_STATUS 0 PROTOS_DL_CODE)
    if(NOT PROTOS_DL_CODE EQUAL 0)
      file(REMOVE "${PROTOS_TGZ}")
      message(FATAL_ERROR "proto tarball download failed: ${PROTOS_DL_STATUS}")
    endif()
  endif()
  set(PROTOS_EXPORT "${CMAKE_CURRENT_BINARY_DIR}/protos-export")
  file(ARCHIVE_EXTRACT INPUT "${PROTOS_TGZ}" DESTINATION "${PROTOS_EXPORT}")
  set(PDF_PROTO_SRC "${PROTOS_EXPORT}/pdf-backend/proto/${PDF_PROTO_REL}")
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
set(QPARSE_ENGINE_TAG "8cff78ac81f8f0d7bfb5d89ca991400f4057b93c" CACHE STRING "engine commit")
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

# ---------------------------------------------------------------------------
# The service binary.
# ---------------------------------------------------------------------------
add_executable(grpc_qparse
  src/main.cpp
  src/qparse_service_impl.cpp)
target_include_directories(grpc_qparse PRIVATE src)
target_include_directories(grpc_qparse SYSTEM PRIVATE "${TOPLEVEL_PREFIX_PATH}/src")
target_compile_definitions(grpc_qparse PRIVATE
  ROOT_PATH="${TOPLEVEL_PREFIX_PATH}" LOGURU_WITH_STREAMS=1)
target_link_libraries(grpc_qparse PRIVATE pdf_backend_proto ${ENGINE_DEPENDENCIES} grpc_qparse_flags)

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(qparse_contract_test
  test/contract_roundtrip_test.cpp
  src/qparse_service_impl.cpp)
target_include_directories(qparse_contract_test PRIVATE src)
target_include_directories(qparse_contract_test SYSTEM PRIVATE "${TOPLEVEL_PREFIX_PATH}/src")
target_compile_definitions(qparse_contract_test PRIVATE
  ROOT_PATH="${TOPLEVEL_PREFIX_PATH}" LOGURU_WITH_STREAMS=1)
target_link_libraries(qparse_contract_test PRIVATE pdf_backend_proto ${ENGINE_DEPENDENCIES} grpc_qparse_flags)
add_test(NAME qparse_contract COMMAND qparse_contract_test
  "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/hello.pdf"
  "${QPARSE_RESOURCES_DIR}")
