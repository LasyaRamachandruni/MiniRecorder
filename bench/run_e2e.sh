#!/usr/bin/env bash
# End-to-end benchmarks. Every number in bench/results comes from this script
# (plus run_micro.sh for the Google Benchmark binaries).
#
#   bench/run_e2e.sh [build-dir] [results-dir]
#
# Scenarios (all data is SYNTHETIC, from the built-in simulator):
#   throughput  unpaced producers, block policy (lossless), zstd 1/3/9:
#               full sensor set (bytes are camera dominated) and small-record
#               only (camera off) to measure the per-record path
#   durability  real-time nominal rates, latency per fsync policy and chunk age
#   overload    sensors paced at 30x nominal (~275 MB/s offered) into zstd 9
#               (~55 MB/s capacity here), per overflow policy
#   recovery    SIGKILL a recorder, then time mr_recover on ~256 MB and ~1 GB files
#   replay      mr_replay schedule lateness at 1x, sleep-only vs. 200 us spin
#
# Kept short on purpose (the reference machine is a shared 2 vCPU VM).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$ROOT/build-gcc}
OUT=${2:-$ROOT/bench/results}
BIN=$BUILD/tools
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
BIN=$(cd "$BIN" && pwd)
# Scratch files use a path relative to the repository so the JSON summaries
# do not record machine-specific absolute paths.
cd "$ROOT"
WORK=${MR_BENCH_WORK:-build-bench-work}
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

log() { echo "[$(date +%H:%M:%S)] $*" >&2; }

drop_caches() {
  sync
  if (echo 3 > /proc/sys/vm/drop_caches) 2>/dev/null; then echo cold; else echo warm; fi
}

# ---------------------------------------------------------------- throughput
for level in 1 3 9; do
  log "throughput zstd-$level"
  "$BIN/mr_record" --out "$WORK/tp.mrlog" --unpaced --policy all=block --duration 8 \
    --zstd-level "$level" --fsync interval:100 --quiet \
    --summary-json "$OUT/throughput_zstd$level.json"
  rm -f "$WORK/tp.mrlog"
  log "throughput small records only, zstd-$level"
  "$BIN/mr_record" --out "$WORK/tp.mrlog" --unpaced --policy all=block --duration 8 \
    --camera off --zstd-level "$level" --fsync interval:100 --quiet \
    --summary-json "$OUT/throughput_small_zstd$level.json"
  rm -f "$WORK/tp.mrlog"
done

# ---------------------------------------------------------------- durability
for age in 500 50; do
  for fsync in chunk interval:100 close; do
    name=$(echo "$fsync" | tr ':' '_')
    log "durability fsync=$fsync chunk-age=$age ms"
    "$BIN/mr_record" --out "$WORK/dur.mrlog" --duration 10 --fsync "$fsync" \
      --chunk-age-ms "$age" --track-latency --quiet \
      --summary-json "$OUT/durability_${name}_age${age}.json"
    rm -f "$WORK/dur.mrlog"
  done
done

# ---------------------------------------------------------------- overload
for policy in drop-newest drop-oldest block; do
  log "overload policy=$policy"
  "$BIN/mr_record" --out "$WORK/ov.mrlog" --rate-scale 30 --policy "all=$policy" \
    --zstd-level 9 --duration 5 --quiet \
    --summary-json "$OUT/overload_$policy.json"
  rm -f "$WORK/ov.mrlog"
done

# ---------------------------------------------------------------- recovery
recovery_case() {
  local label=$1 seconds=$2
  log "recovery: recording ~$label then SIGKILL"
  "$BIN/mr_record" --out "$WORK/rec.mrlog" --unpaced --policy all=block --zstd-level 1 \
    --duration 600 --quiet &
  local pid=$!
  sleep "$seconds"
  kill -9 "$pid"
  wait "$pid" 2>/dev/null || true
  local size
  size=$(stat -c %s "$WORK/rec.mrlog.partial")
  cp "$WORK/rec.mrlog.partial" "$WORK/rec_copy.partial"
  sync
  {
    echo "{\"label\": \"$label\", \"partial_bytes\": $size, \"runs\": ["
    local first=1
    for mode in verify no-verify; do
      for cache in cold warm; do
        cp "$WORK/rec_copy.partial" "$WORK/run.mrlog.partial"
        # Flush the copy first so recovery's own fdatasync does not pay for it.
        sync
        local state=warm
        if [ "$cache" = cold ]; then state=$(drop_caches); fi
        local flags=""
        if [ "$mode" = no-verify ]; then flags="--no-verify"; fi
        local out
        # shellcheck disable=SC2086
        out=$("$BIN/mr_recover" "$WORK/run.mrlog.partial" $flags)
        local secs chunks kept discarded
        secs=$(echo "$out" | awk '/^elapsed:/ {print $2}')
        chunks=$(echo "$out" | awk '/^chunks recovered:/ {print $3}')
        kept=$(echo "$out" | awk '/^bytes kept:/ {print $3}')
        discarded=$(echo "$out" | awk '/^bytes kept:/ {gsub("\\(", "", $6); print $6}')
        "$BIN/mr_inspect" "$WORK/run.mrlog" --verify >/dev/null
        [ $first -eq 1 ] || echo ","
        first=0
        echo "{\"mode\": \"$mode\", \"cache\": \"$state\", \"seconds\": $secs, \"chunks\": $chunks, \"bytes_kept\": $kept, \"bytes_discarded\": $discarded}"
        rm -f "$WORK/run.mrlog"
      done
    done
    echo "]}"
  } > "$OUT/recovery_$label.json"
  rm -f "$WORK/rec.mrlog.partial" "$WORK/rec_copy.partial"
}
recovery_case 256MB 1
recovery_case 1GB 4

# ---------------------------------------------------------------- replay
log "replay: recording 10 s at nominal rates"
"$BIN/mr_record" --out "$WORK/replay.mrlog" --duration 10 --quiet
for spin in 0 200; do
  log "replay 1x spin=$spin us"
  "$BIN/mr_replay" "$WORK/replay.mrlog" --speed 1 --spin-us "$spin" \
    --json "$OUT/replay_1x_spin$spin.json" >/dev/null
done
log "replay as fast as possible"
"$BIN/mr_replay" "$WORK/replay.mrlog" --speed 0 --json "$OUT/replay_max.json" >/dev/null

log "done; results in $OUT"
