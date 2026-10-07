# Reproducible build and test environment (Ubuntu 24.04, GCC 13, Clang 18).
#
#   docker build -t minirecorder .
#   docker run --rm minirecorder                       # runs the test suite
#   docker run --rm -it minirecorder bash              # tools are on PATH
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates \
      clang clang-tidy clang-format libclang-rt-18-dev \
      libzstd-dev libgtest-dev libbenchmark-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake --preset gcc-release \
 && cmake --build --preset gcc-release

ENV PATH="/src/build-gcc-release/tools:${PATH}"
CMD ["ctest", "--preset", "gcc-release"]
