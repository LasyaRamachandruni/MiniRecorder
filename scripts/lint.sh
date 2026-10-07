#!/usr/bin/env bash
# clang-format check on all C++ sources and clang-tidy on the library and
# tools (tests and benchmarks are formatted but not tidied).
#
#   scripts/lint.sh [build-dir-with-compile_commands.json]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build-clang-release}
cd "$ROOT"

echo "== clang-format"
git ls-files '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror

echo "== clang-tidy"
if [ ! -f "$BUILD/compile_commands.json" ]; then
  cmake --preset clang-release >/dev/null
  BUILD=$ROOT/build-clang-release
fi
run-clang-tidy -p "$BUILD" -quiet -j "$(nproc)" 'src/.*\.cpp|tools/.*\.cpp'
echo "lint: OK"
