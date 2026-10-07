#!/usr/bin/env bash
# Runs the Google Benchmark binaries and stores JSON + console output.
#
#   bench/run_micro.sh [build-dir] [results-dir]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build-gcc}
OUT=${2:-$ROOT/bench/results}
mkdir -p "$OUT"

for b in bench_ring bench_codec; do
  echo "== $b" >&2
  "$BUILD/bench/$b" --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
    --benchmark_min_time=0.2s --benchmark_out="$OUT/micro_${b#bench_}.json" \
    --benchmark_out_format=json | tee "$OUT/micro_${b#bench_}.txt"
done
