#!/usr/bin/env bash
# End-to-end smoke test: record, SIGKILL mid-write, recover, inspect, replay.
#
#   scripts/e2e_smoke.sh [path/to/build/tools]
set -euo pipefail

BIN=${1:-build/tools}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "== clean 3 s recording"
"$BIN/mr_record" --out "$WORK/clean.mrlog" --duration 3 --camera 320x240@30 --quiet
"$BIN/mr_inspect" "$WORK/clean.mrlog" --verify

echo "== record for 5 s, then SIGKILL"
"$BIN/mr_record" --out "$WORK/crash.mrlog" --duration 60 --fsync interval:50 \
  --faults gap=0.002,regress=0.002 --quiet &
PID=$!
sleep 5
kill -9 "$PID"
wait "$PID" || true
test -f "$WORK/crash.mrlog.partial"
test ! -f "$WORK/crash.mrlog"

echo "== recover"
"$BIN/mr_recover" "$WORK/crash.mrlog.partial"
test -f "$WORK/crash.mrlog"

echo "== inspect + verify recovered file"
"$BIN/mr_inspect" "$WORK/crash.mrlog" --events --limit 10 --verify

echo "== replay (as fast as possible)"
"$BIN/mr_replay" "$WORK/crash.mrlog" --speed 0 --sink checksum

echo "e2e smoke: OK"
