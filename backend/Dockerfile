FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends build-essential cmake pkg-config ca-certificates libdrogon-dev libsimdjson-dev libjsoncpp-dev libpqxx-dev libssl-dev libgtest-dev libsqlite3-dev libmariadb-dev libmariadb-dev-compat libbrotli-dev libhiredis-dev libyaml-cpp-dev uuid-dev zlib1g-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /source/backend
COPY backend/ ./
COPY tests/fixtures /source/tests/fixtures
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTING=ON && cmake --build build --parallel 2 && ctest --test-dir build --output-on-failure
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends libdrogon-dev libsimdjson-dev libjsoncpp-dev libpqxx-dev libssl-dev ca-certificates curl && rm -rf /var/lib/apt/lists/* && useradd -m -u 10001 lens && mkdir -p /jobs /uploads && chown 10001:10001 /jobs /uploads
WORKDIR /app
COPY --from=build /source/backend/build/decision_lens /app/decision_lens
COPY database/migrations /app/database/migrations
COPY backend/src/core.cpp /app/native/core.cpp
COPY python-worker/procedures /app/python-worker/procedures
COPY python-worker/checks /app/python-worker/checks
USER lens
EXPOSE 8080
ENTRYPOINT ["/app/decision_lens"]





