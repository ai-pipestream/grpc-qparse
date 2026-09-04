# syntax=docker/dockerfile:1.26
# grpc-qparse image: the qpdf-based cell-parser PDF backend (amd64 only,
# like the family's other C++ services).
#
# The build stage compiles the service and the commit-pinned engine with
# its dependency set from source and runs the contract test; the test gates
# the image. The runtime stage carries only the binary and the engine's
# font resources, which the service must find through GRPC_QPARSE_RESOURCES
# or decode throws.

# The image tag passed by the publish workflow; becomes build_version in
# GetServiceInfo.
ARG GRPC_QPARSE_BUILD_VERSION=dev

FROM ubuntu:26.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ git make pkg-config zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

ARG GRPC_QPARSE_BUILD_VERSION

# The contract protos are downloaded from the pinned parser-protos commit
# at configure time (sha256-verified), so the build needs network access.
# Unix Makefiles, not Ninja: the engine's extlib ExternalProjects do not
# declare BUILD_BYPRODUCTS, which Ninja requires.
RUN --mount=type=cache,id=grpc-qparse-ubuntu26-grpc1.83.1-make,target=/build \
    cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release \
        -DGRPC_QPARSE_BUILD_VERSION=${GRPC_QPARSE_BUILD_VERSION} \
    && cmake --build /build --parallel \
    && ctest --test-dir /build --output-on-failure \
    && mkdir -p /out && cp /build/grpc_qparse /out/ \
    && cp -r /build/pdf_resources /out/pdf_resources

FROM ubuntu:26.04

COPY --from=build /out/grpc_qparse /usr/local/bin/grpc_qparse
COPY --from=build /out/pdf_resources /usr/local/share/grpc-qparse/pdf_resources

ENV GRPC_QPARSE_PORT=50070 \
    GRPC_QPARSE_RESOURCES=/usr/local/share/grpc-qparse/pdf_resources

# The service keeps documents in memory only; run with --read-only and a
# tmpfs /tmp:
#   docker run --rm --read-only --tmpfs /tmp -p 50070:50070 grpc-qparse
USER 65532:65532
EXPOSE 50070
ENTRYPOINT ["/usr/local/bin/grpc_qparse"]
