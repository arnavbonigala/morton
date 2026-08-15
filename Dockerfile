FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        g++ cmake make ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY apps ./apps
COPY tests ./tests

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    && cmake --build build -j"$(nproc)" --target morton_world morton_matchmaker morton_loadtest

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/apps/morton_world /usr/local/bin/
COPY --from=build /src/build/apps/morton_matchmaker /usr/local/bin/
COPY --from=build /src/build/apps/morton_loadtest /usr/local/bin/

ENTRYPOINT ["/usr/local/bin/morton_world"]
