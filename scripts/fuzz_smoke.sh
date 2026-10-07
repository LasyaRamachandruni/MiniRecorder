#!/usr/bin/env bash
# Builds the libFuzzer target with ASan+UBSan and runs it for a short time.
#
#   scripts/fuzz_smoke.sh [seconds]   (default 60)
set -euo pipefail

SECONDS_TO_RUN=${1:-60}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="$ROOT/build-fuzz"

CXX=clang++ cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMR_BUILD_FUZZERS=ON -DMR_BUILD_TESTS=OFF -DMR_BUILD_BENCHMARKS=OFF \
  -DMR_SANITIZE=address,undefined >/dev/null
cmake --build "$BUILD" --target fuzz_chunk fuzz_make_seeds

CORPUS="$BUILD/corpus"
mkdir -p "$CORPUS"
"$BUILD/fuzz/fuzz_make_seeds" "$CORPUS"
"$BUILD/fuzz/fuzz_chunk" "$CORPUS" -max_total_time="$SECONDS_TO_RUN" -max_len=4096 \
  -rss_limit_mb=2048 -print_final_stats=1
