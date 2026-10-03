# syntax=docker/dockerfile:1.27
# grpc-qparse image: the qpdf-based cell-parser PDF backend, published as a
# linux/amd64 + linux/arm64 manifest list (each leg built and smoke-tested
# natively on its own architecture).
#
# The build stage compiles the service and the commit-pinned engine with
# its dependency set from source and runs the contract test; the test gates
# the image. The runtime stage is a hardened, glibc-only base: no package
# manager, no ldconfig run, and no shell needed. It carries the binary, the
# engine's font resources (which the service must find through
# GRPC_QPARSE_RESOURCES or decode throws), the shared libraries the binary
# needs beyond glibc (libstdc++, libgcc_s, libz), staged from the build
# stage into /usr/local/lib by scripts/stage-runtime-libs.sh, which fails
# the build if anything would still resolve from outside it, and the
# third-party license texts under /usr/local/share/doc/grpc-qparse.
#
# The build stage is Debian trixie on purpose: the runtime base's glibc is
# 2.41, and a binary linked against a newer glibc (ubuntu 26.04's) refuses
# to load there. The base is swappable for any image whose glibc is 2.41 or
# newer:
#   --build-arg GRPC_QPARSE_RUNTIME_IMAGE=<image>
ARG GRPC_QPARSE_RUNTIME_IMAGE=dhi.io/debian-base:trixie-debian13

# The image tag passed by the publish workflow; becomes build_version in
# GetServiceInfo.
ARG GRPC_QPARSE_BUILD_VERSION=dev

FROM debian:trixie-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ git make pkg-config zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

ARG GRPC_QPARSE_BUILD_VERSION

# The contract protos are downloaded from the pinned parser-protos commit
# at configure time (sha256-verified), so the build needs network access.
# Unix Makefiles, not Ninja: the engine's extlib ExternalProjects do not
# declare BUILD_BYPRODUCTS, which Ninja requires. The cache id encodes every
# ABI-sensitive dependency; bump it when gRPC or the toolchain moves. The id
# also keys on the target architecture: multi-arch publish legs share cache
# mounts by id, so an unkeyed id would let the arm64 leg reuse amd64 objects.
# Compile parallelism is bounded: an unbounded build on a shared builder
# starves its neighbours and gets the compiler OOM-killed; 8 jobs is what
# the gRPC compile tolerates beside other builds.
# A second builder sharing this cache mount (a developer build beside a CI
# run, or an interrupted build that left a truncated object behind) gets its
# own tree through --build-arg GRPC_QPARSE_BUILD_CACHE_SCOPE=-<name>.
# TARGETARCH is a global-scope platform argument: without this declaration
# it expands to nothing inside the stage, and both architectures would share
# one cache tree. The engine, contract and font pins are plain CMake
# variables, so a reused tree always configures the pinned commits.
ARG TARGETARCH
ARG GRPC_QPARSE_BUILD_CACHE_SCOPE=
RUN --mount=type=cache,id=grpc-qparse-trixie-grpc1.83.1-make-${TARGETARCH}${GRPC_QPARSE_BUILD_CACHE_SCOPE},target=/build \
    cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
        -DGRPC_QPARSE_BUILD_VERSION=${GRPC_QPARSE_BUILD_VERSION} \
    && cmake --build /build --parallel 8 \
    && ctest --test-dir /build --output-on-failure \
    && mkdir -p /out/lib && cp /build/grpc_qparse /out/ \
    && cp -r /build/pdf_resources /out/pdf_resources \
    && scripts/stage-runtime-libs.sh /out/lib /out/grpc_qparse \
    && scripts/collect-notices.sh /build /src /out/doc

# LD_LIBRARY_PATH stands in for ldconfig, and the numeric USER works with or
# without a passwd entry (65532 is the conventional nonroot uid in hardened
# images).
FROM ${GRPC_QPARSE_RUNTIME_IMAGE}

COPY --from=build /out/lib/ /usr/local/lib/
COPY --from=build /out/grpc_qparse /usr/local/bin/grpc_qparse
COPY --from=build /out/pdf_resources /usr/local/share/grpc-qparse/pdf_resources
# LICENSE, NOTICE and every redistributed component's license text
# (scripts/collect-notices.sh).
COPY --from=build /out/doc/ /usr/local/share/doc/grpc-qparse/

ENV GRPC_QPARSE_PORT=50070 \
    GRPC_QPARSE_RESOURCES=/usr/local/share/grpc-qparse/pdf_resources \
    LD_LIBRARY_PATH=/usr/local/lib

# The service keeps documents in memory only and writes nothing, so the
# container runs read-only as it is:
#   docker run --rm --read-only -p 50070:50070 grpc-qparse
USER 65532:65532
EXPOSE 50070
ENTRYPOINT ["/usr/local/bin/grpc_qparse"]
