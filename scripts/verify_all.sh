#!/usr/bin/env bash
# Runs every verification step CI runs and writes a plain-text report.
#
#   scripts/verify_all.sh [report-path]   (default bench/results/verification.txt)
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
REPORT=${1:-$ROOT/bench/results/verification.txt}
cd "$ROOT"

status=0
{
  echo "MiniRecorder verification run"
  echo "date:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit: $(git rev-parse --short HEAD)"
  echo "host:   $(lscpu | awk -F: '/Model name/ {gsub(/^ +/, "", $2); print $2}'), $(nproc) vCPU"
  echo
  for preset in gcc-release clang-release asan tsan; do
    cmake --preset "$preset" >/dev/null && cmake --build --preset "$preset" >/dev/null 2>&1
    export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1
    export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
    export TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1
    result=$(ctest --preset "$preset" 2>&1 | grep -E "tests passed|tests failed")
    echo "ctest --preset $preset: $result"
    echo "$result" | grep -q "100% tests passed" || status=1
  done
  echo
  if scripts/lint.sh >/tmp/mr_lint.log 2>&1; then
    echo "scripts/lint.sh: clang-format clean on all C++ files; clang-tidy clean on src/ and tools/"
  else
    echo "scripts/lint.sh: FAILED (see /tmp/mr_lint.log)"
    status=1
  fi
  echo
  echo "scripts/fuzz_smoke.sh 60 (libFuzzer + ASan + UBSan):"
  if scripts/fuzz_smoke.sh 60 >/tmp/mr_fuzz.log 2>&1; then
    grep -E "^stat::(number_of_executed_units|average_exec_per_sec|new_units_added|peak_rss_mb)" /tmp/mr_fuzz.log | sed 's/^/  /'
    grep -E "DONE" /tmp/mr_fuzz.log | sed 's/^/  /'
    echo "  no crashes, leaks or sanitizer reports"
  else
    echo "  FAILED (see /tmp/mr_fuzz.log)"
    status=1
  fi
  echo
  echo "SIGKILL crash test (build-gcc-release/tests/test_crash_kill):"
  build-gcc-release/tests/test_crash_kill --gtest_output=xml:/tmp/mr_crash.xml >/dev/null 2>&1 || status=1
  grep -oE '<testcase name="[^"]+"|<property name="[a-z_]+" value="[^"]+"' /tmp/mr_crash.xml |
    sed -E 's/<testcase name="([^"]+)"/  \1:/; s/<property name="([a-z_]+)" value="([^"]+)"/    \1 = \2/'
} | tee "$REPORT"
exit $status
