# syntax=docker/dockerfile:1
#
# Builds LodLightRecolor.asi (a Windows x64 DLL) from a Linux container with
# mingw-w64. Nothing needs to be installed on the host besides Docker.
#
#   docker build --target export -o dist .
#
# produces dist/LodLightRecolor.asi and dist/lodlight_recolor.ini.

FROM debian:bookworm-slim AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates mingw-w64 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt lodlight_recolor.ini ./
COPY cmake/ cmake/
COPY src/ src/
COPY res/ res/
COPY third_party/ third_party/
COPY tests/ tests/

# 1. Colour-math tests, compiled natively for the container. A failure here
#    stops the build before anything is cross-compiled.
RUN cmake -S . -B build-tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DLODLIGHT_BUILD_TESTS=ON \
 && cmake --build build-tests \
 && ctest --test-dir build-tests --output-on-failure

# 2. The plugin itself.
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
 && cmake --build build

# 3. Collect outputs and print the DLL's imports so a stray libstdc++/libgcc
#    dependency is visible in the build log.
RUN mkdir -p /out \
 && cp build/LodLightRecolor.asi lodlight_recolor.ini /out/ \
 && x86_64-w64-mingw32-objdump -p /out/LodLightRecolor.asi | grep -E 'DLL Name'

FROM scratch AS export
COPY --from=build /out/ /
