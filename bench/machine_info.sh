#!/usr/bin/env bash
# Records the hardware and toolchain a benchmark run used.
#
#   bench/machine_info.sh [results-dir]
set -uo pipefail

OUT=${1:-$(cd "$(dirname "$0")" && pwd)/results}
mkdir -p "$OUT"
DATA_DIR=$(cd "$(dirname "$0")/.." && pwd)
DEV=$(df --output=source "$DATA_DIR" | tail -1)
BASE=$(basename "$DEV" | sed 's/[0-9]*$//')
{
  echo "date:        $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "kernel:      $(uname -sr)"
  echo "cpu model:   $(lscpu | awk -F: '/Model name/ {gsub(/^ +/, "", $2); print $2}')"
  echo "vcpus:       $(nproc)"
  echo "hypervisor:  $(lscpu | awk -F: '/Hypervisor vendor/ {gsub(/^ +/, "", $2); print $2}')"
  echo "memory:      $(free -g | awk '/Mem:/ {print $2 " GiB"}')"
  echo "data device: $DEV ($(df -T "$DATA_DIR" | awk 'NR==2 {print $2}'), rotational flag $(cat /sys/block/"$BASE"/queue/rotational 2>/dev/null || echo '?'))"
  echo "             virtio block device; the physical media behind it is not visible from the guest"
  echo "gcc:         $(g++ --version | head -1)"
  echo "clang:       $(clang++ --version | head -1)"
  echo "zstd:        $(dpkg-query -W -f='${Version}' libzstd-dev 2>/dev/null || echo unknown)"
  echo "benchmark:   $(dpkg-query -W -f='${Version}' libbenchmark-dev 2>/dev/null || echo unknown)"
  echo "build:       CMake Release (-O3 -DNDEBUG), GCC unless noted"
  echo
  echo "lscpu:"
  lscpu | sed 's/^/  /' | grep -vE "Flags|Vulnerability"
} > "$OUT/machine.txt"
cat "$OUT/machine.txt"
