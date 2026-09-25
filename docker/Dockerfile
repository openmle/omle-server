# omle-server — OIP inference server
#
# Two stages: a builder that compiles the dependencies from pinned source, and
# a runtime that carries the resulting executable and almost nothing else.
#
# Build from the repository root, not from this directory — the context has to
# contain the sources this copies:
#
#   docker build -f docker/Dockerfile -t omle-server .
#
# .dockerignore stays at the root for the same reason: the classic builder reads
# it from the context directory, so a copy placed beside this file is ignored.
#
# The build needs three repositories side by side. omle-server's CMakeLists
# adds omle-runtime as a subproject, and omle-runtime generates omle.pb.cc from
# the schema in omle, so both are cloned here at the same refs .github/workflows
# /publish.yml pins. Override them to test a different pairing:
#
#   docker build -f docker/Dockerfile \
#       --build-arg OMLE_RUNTIME_REF=v0.1.0 -t omle-server .
#
# Debian rather than Alpine: the bundled build links the system OpenSSL, and
# Alpine's musl would also mean rebuilding every dependency against a libc the
# project does not otherwise test.

# ── builder ──────────────────────────────────────────────────────────────────
FROM debian:12-slim AS builder

# Sibling sources default to the tip of their default branch, which is what
# every other job in test.yml builds against: those check out openmle/omle-runtime
# and openmle/omle with no `ref:`. Pinning releases here instead would leave the
# docker job validating a combination nothing else in CI exercises, and would
# fail outright whenever this repository needs a runtime change that is not yet
# released.
#
# Releases do not use these defaults. publish.yml overrides both build-args from
# its workflow-level env, the same values it checks the siblings out at for the
# wheels, so a published image is built from fixed refs and matches the wheels
# shipped beside it.
ARG OMLE_RUNTIME_REF=main
ARG OMLE_REF=main

# Only the toolchain and the three libraries the bundled build resolves against
# the system. gRPC, Abseil, protobuf, Drogon, jsoncpp and simdjson are compiled
# from pinned source by cmake/bundled_deps.cmake and must NOT be installed:
# finding a system copy is the failure that build exists to prevent.
#
# OpenSSL is external by design — a vendored libssl stops receiving the
# distribution's security updates. uuid and zlib are Drogon's: its CMakeLists
# calls find_package on both even when built from source.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
        libssl-dev uuid-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Shallow clones of the two siblings, at the pinned refs. Copied before the
# server sources so a change to this repository does not invalidate them.
RUN git clone --depth 1 --branch "${OMLE_RUNTIME_REF}" \
        https://github.com/openmle/omle-runtime.git omle-runtime \
 && git clone --depth 1 --branch "${OMLE_REF}" \
        https://github.com/openmle/omle.git omle

COPY . /src/omle-server

WORKDIR /src/omle-server

# OMLE_SERVER_BUNDLE_DEPS=ON links every dependency statically, leaving only
# OpenSSL and system libraries external. Without it the binary would need gRPC,
# Abseil, protobuf and Drogon installed at matching versions in the runtime
# image — 112 shared libraries — which defeats the point of a small final stage.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DOMLE_SERVER_BUNDLE_DEPS=ON \
 && cmake --build build --target omle_server -j "$(nproc)"

# A binary still naming gRPC, Abseil, protobuf or Drogon means the bundled build
# silently found a system copy. Such an image starts and only fails on a host
# without those libraries, so it is caught here instead.
RUN LEAKED=$(ldd build/omle_server \
        | grep -ciE 'grpc|absl|protobuf|drogon|trantor|simdjson|omleruntime' || true) \
 && if [ "$LEAKED" -ne 0 ]; then \
        echo "build/omle_server links $LEAKED libraries it should have absorbed"; \
        ldd build/omle_server; exit 1; \
    fi \
 && echo "omle_server absorbed its dependencies"

# ── runtime ──────────────────────────────────────────────────────────────────
FROM debian:12-slim

# libssl3 and libuuid1 are what the executable still resolves at load time;
# zlib1g and libstdc++6 come with the base image. curl is here only for the
# HEALTHCHECK below — a port probe would report ready while the model registry
# was still empty, which is the state worth knowing about.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libssl3 libuuid1 curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/omle-server/build/omle_server /usr/local/bin/omle-server

# Unprivileged: the server binds 8080/8081, both above 1024, so nothing here
# needs root. /models is owned by the same user so a mounted volume stays
# readable after `docker run --user`.
RUN useradd --system --create-home --uid 10001 omle \
 && mkdir -p /models \
 && chown omle:omle /models
USER omle

# Defaults, all overridable — by environment, or by flags after the image name:
#   docker run -v ./models:/models -p 8080:8080 omle-server
#   docker run ... omle-server --rest-port 9000 --batching
ENV OMLE_MODEL_DIR=/models \
    OMLE_REST_PORT=8080 \
    OMLE_GRPC_PORT=8081

VOLUME ["/models"]
EXPOSE 8080 8081

# /v2/health/ready, not /live: ready reflects the registry having loaded, so a
# container pointed at an empty or unreadable model directory reports unhealthy
# rather than accepting traffic it cannot serve.
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -fsS "http://127.0.0.1:${OMLE_REST_PORT}/v2/health/ready" || exit 1

ENTRYPOINT ["/usr/local/bin/omle-server"]
CMD ["serve"]
