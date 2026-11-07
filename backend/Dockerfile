FROM ubuntu:24.04 AS runtime-deps
ARG TARGETARCH
RUN --mount=type=cache,id=lens-noble-apt-lists-${TARGETARCH},target=/var/lib/apt/lists,sharing=locked \
    apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl libdrogon1t64 libjsoncpp25 libpqxx-7.8t64 libsimdjson19 libssl3t64 libxml2

FROM runtime-deps AS toolchain
RUN --mount=type=cache,id=lens-noble-apt-lists-${TARGETARCH},target=/var/lib/apt/lists,sharing=locked \
    apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        g++ cmake ninja-build pkg-config ccache libxml2-dev libdrogon-dev libsimdjson-dev \
        libjsoncpp-dev libpqxx-dev libssl-dev libgtest-dev libsqlite3-dev libmariadb-dev \
        libmariadb-dev-compat libbrotli-dev libhiredis-dev libyaml-cpp-dev uuid-dev zlib1g-dev

FROM toolchain AS build
ARG BUILD_JOBS=2
ARG TARGETARCH
ENV CCACHE_DIR=/var/cache/ccache CCACHE_COMPILERCHECK=content CCACHE_NODIRECT=true \
    CCACHE_EXTRAFILES=/tmp/gcc-components.sha256 CCACHE_STATSLOG=/tmp/ccache-stats.log
RUN --mount=type=bind,source=backend,target=/source/backend \
    --mount=type=bind,source=tests/fixtures,target=/source/tests/fixtures \
    --mount=type=cache,id=lens-gcc-noble-${TARGETARCH},target=/var/cache/ccache \
    sha256sum "$(g++ -print-prog-name=cc1plus)" "$(command -v as)" > /tmp/gcc-components.sha256 \
    && cmake -S /source/backend -B /tmp/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=g++ -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DBUILD_TESTING=ON -DBUILD_BENCHMARK=OFF \
    && cmake --build /tmp/build --parallel "${BUILD_JOBS}" \
    && ctest --test-dir /tmp/build --output-on-failure \
    && ccache --show-log-stats \
    && install -D /tmp/build/decision_lens /out/decision_lens \
    && rm -rf /tmp/build

FROM runtime-deps
RUN useradd -m -u 10001 lens && mkdir -p /jobs /uploads && chown 10001:10001 /jobs /uploads
WORKDIR /app
COPY database/migrations /app/database/migrations
COPY python-worker/procedures /app/python-worker/procedures
COPY python-worker/checks /app/python-worker/checks
COPY backend/src/core.cpp /app/native/core.cpp
COPY --from=build /out/decision_lens /app/decision_lens
USER lens
EXPOSE 8080
ENTRYPOINT ["/app/decision_lens"]
