# MiniRecorder design

This document explains how the recorder is put together and why. The on-disk
format is specified separately in [FORMAT.md](FORMAT.md); measured numbers
are in [bench/results/RESULTS.md](../bench/results/RESULTS.md).

## Goals and non-goals

Goals:

* Never block a sensor thread on I/O, compression or locks held by other
  threads; make every loss explicit and counted.
* A crash (process kill or power loss) costs at most the data that was not
  yet durable, and a crashed file can always be turned back into a readable
  one.
* Bounded memory everywhere: rings, pre-trigger buffer, in-flight chunks.
* Fast, ordered reads for replay and inspection.

Non-goals (see "Limitations"): multi-host recording, network transport,
schema/IDL for payloads, encryption.

## Threads

```
 sensor threads (1 per stream)        collector (1)               storage writer
 ─────────────────────────────        ─────────────               ──────────────
 imu    ──push──> [ RecordRing ] ─┐
 gps    ──push──> [ RecordRing ] ─┤   pop, copy out,             ┌─> zstd worker ─┐
 wheel  ──push──> [ RecordRing ] ─┼─> quality checks, privacy, ──┼─> zstd worker ─┼─> reorder ──> writer thread
 camera ──push──> [ RecordRing ] ─┘   trigger / event window,    │   (N threads)  │   buffer      write(2),
                                      append to chunk builder ───┘                │   by seq      fdatasync,
                                      (seal: bounded semaphore)                   │               chunk index
```

| Thread | Blocks on | Never blocks on |
|---|---|---|
| sensor | its own ring only, and only with the `block` policy | disk, compression, other sensors |
| collector | the in-flight chunk semaphore (disk back-pressure); 200 us idle sleep when all rings are empty | any sensor |
| zstd worker | job queue (mutex + condvar, one job per ~1 MiB chunk) | disk |
| writer | ready-chunk map (mutex + condvar), `write(2)`, `fdatasync` | compression |

With the default sensor set this is 4 + 1 + 2 + 1 = 8 threads; the reference
machine has 2 vCPUs, which is one reason the producers sleep between samples
and the collector sleeps when idle instead of spinning.

## Data path and copies

1. The producer reserves a ring slot and generates the payload directly into
   it (`RecordRing::push_with`), so producing is allocation free and copy free.
2. The collector copies the record out of the slot into a scratch buffer and
   releases the slot immediately. This is one memcpy (`bench_ring` moves
   640x480 frames through a ring, filled in place and copied out, at about
   59 k frames/s, i.e. roughly 17 us per frame) but it means a producer never waits for quality
   checks, privacy filtering, trigger logic or a blocked writer, and it keeps
   the drop-oldest lock window (below) short.
3. The writer appends the record to the per-stream chunk builder (second copy),
   which is the buffer that gets compressed.

Chunk builders, compressed outputs and pre-trigger copies use recycled
buffers, so steady-state operation does not allocate per record or per chunk.

## Ring buffers and memory ordering

### SpscQueue / RecordRing index protocol

`head_` is written only by the producer, `tail_` only by the consumer. Both
are 64-bit counters that only increase (they would take centuries to wrap at
a billion operations per second); the slot is `counter & (capacity - 1)`,
which is why capacity is a power of two.

Producer:

```
h = head_.load(relaxed)                 // own variable
if h - cached_tail == capacity:
    cached_tail = tail_.load(acquire)   // (A) pairs with (D)
    if still full: overflow policy
write slot[h & mask]
head_.store(h + 1, release)             // (B) publishes the slot
```

Consumer:

```
t = tail_.load(relaxed)                 // own variable
if t >= cached_head:
    cached_head = head_.load(acquire)   // (C) pairs with (B)
    if still empty: return false
read slot[t & mask]
tail_.store(t + 1, release)             // (D) hands the slot back
```

(B)->(C) makes the slot contents visible before the consumer reads them;
(D)->(A) makes the consumer's read of a slot happen before the producer
overwrites it. Nothing needs sequential consistency because each variable has
one writer and the only cross-variable reasoning is "slot then index", which
release/acquire provides. Each side keeps a cached copy of the other side's
index and only touches the shared line when the cache says full/empty, and
`head_`, `tail_` and both caches live on separate 64-byte lines, so in the
steady state the two cores do not bounce a line on every operation.
`bench_ring` measures this against a `std::mutex` + `std::queue` with the
same spin structure: 15.8 M vs 2.9 M messages/s, and 522 ns vs 2,185 ns
median round trip.

### Drop-oldest makes `tail_` a two-writer variable

With `drop-oldest`, a producer that finds the ring full evicts the oldest
record by advancing `tail_` itself. That breaks the single-writer property of
`tail_`, so the consumer has to claim a slot before reading it:

```
consumer:  CAS tail_: t -> t | LOCK   (acq_rel)   // claim slot t
           read slot t
           tail_.store(t + 1, release)           // release, clears LOCK
producer:  t = tail_.load(acquire)
           if t has LOCK: back off and retry     // consumer is reading the oldest slot
           CAS tail_: t -> t + 1 (acq_rel)       // evict slot t
           write slot (head & mask)              // == t & mask, the one just evicted
```

Why this is safe:

* The consumer reads slot `t` only while `tail_ == t | LOCK`. The producer's
  eviction CAS expects an unlocked value, so it cannot succeed during the
  read, and the slot the producer writes next (`head & mask`) is exactly the
  evicted one, which the consumer will never read.
* If the producer evicts first, the consumer's CAS fails, it reloads `tail_`
  and retries with the new oldest record. Because eviction can move `tail_`
  past the consumer's cached head, the empty check is `t >= cached_head`, not
  `==`, which forces a fresh acquire of `head_` before reading.
* The producer only waits while the consumer holds the lock, i.e. for the
  duration of one memcpy out of the slot; that is why step 2 above matters.

The stress test pushes 200,000 variable-size records through a 64-slot ring
under each policy with a consumer that stalls periodically, and checks payload
integrity, strict FIFO order and `received + dropped + evicted == produced`.
It passes under ThreadSanitizer.

### Block policy

A full ring with `block` makes the producer back off (64 x `pause`, then
`yield`, then 50 us sleeps) and re-check `tail_`; time spent waiting is
counted per stream. C++20 `atomic::wait/notify` was not used because it would
put a `notify` (a potential futex syscall) on every consumer pop.
`RecordRing::close()` wakes blocked producers so shutdown cannot hang.

## Collector

* Round-robin over rings, at most 64 records per ring per pass, so one busy
  stream cannot starve the others in the collector.
* Per record: quality checks, privacy filter, then either append to storage
  (continuous mode) or hand to the event window (triggered mode), then the
  trigger detector.
* Every 10 ms: stale-stream check, ring-loss accounting (converted into
  `records_dropped` events), sealing of chunks older than `chunk_max_age`.
* When every ring is empty it sleeps 200 us. That bounds the latency it adds
  and keeps an idle recorder at near-zero CPU on a small machine.

Quality events and trigger events are written as records on stream 63. They
are therefore ordered with the data, compressed, CRC protected, and survive a
crash; the footer counters are derived from them (`accumulate_event`), which
is how recovery restores them.

## Storage writer

### Per-stream chunks

Records are batched per stream. A chunk is sealed when it reaches
`chunk_target_bytes` (1 MiB default) or when its first record is older than
`chunk_max_age` (500 ms default). Single-stream chunks:

* keep similar data together, which is what an LZ compressor exploits (per
  stream type at zstd 3: 3.2x for wheel speed frames, 1.9x for GPS, 1.4x for
  IMU, 1.3x for camera frames; an interleaved layout was not measured),
* let a reader that wants one stream skip every other chunk without
  decompressing it,
* make per-stream time-range seeks a lookup in a per-stream index.

### Parallel compression, single ordered writer

Sealed chunks get a monotonically increasing `chunk_seq` and go to a small
pool of zstd workers (one `ZSTD_CCtx` per worker, reused). Workers finish in
any order and put results in a `std::map` keyed by `chunk_seq`; the single
writer thread only takes the entry whose key is the next expected sequence
number. Consequences:

* Compression scales with cores while the file is still written strictly in
  order with one `write(2)` per chunk and one owner of the file offset and the
  index; no locking around the file.
* The file is always "a valid prefix plus at most one torn chunk", which is
  what makes recovery a simple forward scan.

### Back-pressure

A `std::counting_semaphore` (default 8 permits) is acquired when a chunk is
sealed and released after it was written. If compression or the disk falls
behind, sealing blocks, so the collector stops draining rings, rings fill up,
and each stream's overflow policy decides what is lost. Memory stays bounded
by `rings + 8 chunks + builders`.

The measured downside: the semaphore counts chunks regardless of stream, so
a backlog of camera chunks also stalls tiny high-value streams. In the
overload benchmark (sensors at 30x nominal into zstd level 9) the IMU lost
58% of its records under `drop-newest` even though it is a tiny fraction of
the bytes (the camera lost 78%). Priority-aware shedding (camera first) is listed under next steps.

### Durability

| Policy | What is durable | Cost (10 s at nominal rates, 50 ms chunk age) |
|---|---|---|
| `chunk` | every chunk, before the next one is written | 585 fdatasync calls, p99 enqueue-to-durable 67 ms |
| `interval:N` | everything written more than about N ms ago | 99 calls at N = 100, p99 165 ms |
| `close` | nothing until close (a crash keeps whatever the page cache had flushed) | 2 calls, p99 10 s |

Other rules:

* The file header is synced, and the parent directory fsynced, before any
  chunk is written, so a crash leftover always has a header and a name.
* `fdatasync` is used rather than `fsync`: the file size is part of the data
  metadata that `fdatasync` flushes, timestamps are not needed.
* Close: write footer and trailer, `fdatasync`, `close` (which can report
  delayed write-back errors), `rename(.partial -> final)`, fsync the
  directory. A file with its final name is therefore always complete.
* Batching dominates latency: with 500 ms chunk age the median time until a
  record is *written* is about 227 ms under every policy, and the median
  durable latency is 227 ms with per-chunk fsync and 275 ms with a 100 ms
  interval. With 50 ms chunk age the per-chunk median drops to 33 ms. Chunk
  age is the knob that trades latency for compression ratio and index size.

### Why CRCs per chunk

* A chunk is the unit of compression, of writing and of recovery, so it is
  the natural unit of integrity. Per-record CRCs would add 4 bytes and a
  pass over every 48-byte IMU sample; one per-file CRC could not tell which
  part is damaged.
* The header has its own CRC so a torn or corrupted body cannot make the size
  fields lie: recovery trusts `compressed_size` only after the header CRC
  passed, and trusts the body only after the body CRC passed.
* CRC32C has hardware support (SSE4.2: 4.8 GB/s here versus 1.75 GB/s for
  the portable slice-by-8 version, see `bench_codec`), is what iSCSI, ext4
  and many storage engines use, and detects all burst errors up to 32 bits.

## Read path

* `Reader` maps the file read-only (`MADV_SEQUENTIAL`), validates header,
  trailer, footer and index, and never reads past the mapped size.
* Seek by time: for each stream it keeps the chunk list and a running maximum
  of chunk end times. A binary search over the running maximum finds the
  first chunk that can hold a record at or after `t` in O(log n). Using the
  running maximum (not each chunk's own maximum) keeps this correct when
  injected timestamp regressions make chunk ranges overlap; there is a test
  for exactly that.
* `MessageIterator` holds one cursor per selected stream and merges them with
  a binary heap keyed by (timestamp, stream id): O(log k) per record for k
  streams. A cursor is advanced lazily, on the next call, so the payload span
  handed out last stays valid until then even if advancing loads the next
  chunk into the cursor's buffer.

## Quality checks and triggers

* Sequence gaps and timestamp regressions are checked per record.
* Rate is measured over 1 s windows of *sensor* time, so it measures the
  sensor, not scheduling of the recorder, and it is reported only when the
  state changes (out of bounds / recovered) to keep event volume bounded.
* Staleness is measured on *arrival* time in the collector's 10 ms
  housekeeping, because a stalled sensor produces no timestamps at all.
* Hard brake: wheel speed derivative over a 100 ms baseline, or an
  exponentially smoothed IMU longitudinal acceleration, against a threshold
  (5 m/s^2 default), with a 2 s refractory period.
* Event window: a byte-bounded deque of copies of the last `pre` seconds of
  records (default 64 MiB cap); on a trigger the records inside the window are
  persisted with the `pre-trigger` flag and everything until `trigger + post`
  goes straight to storage. A trigger inside an open window extends it.
* The privacy filter runs before buffering, so raw GPS positions and masked
  image regions never reach memory that outlives the collector's scratch
  buffer, and never reach disk.

## Failure modes

| Failure | Effect | Handling |
|---|---|---|
| Process crash / SIGKILL | `.partial` file without footer; possibly a torn last chunk | `mr_recover`; tested by killing the real binary (`test_crash_kill`) |
| Power loss | data since the last `fdatasync` may be lost or partially written | recovery keeps the longest valid prefix; fsync policy bounds the loss |
| Torn or corrupted chunk | header or body CRC mismatch | reader reports `corrupt`; recovery truncates there |
| Corrupt length fields | could cause huge allocations or out-of-range reads | all sizes capped and bounds checked before use; fuzzed |
| Disk full / I/O error | `write`/`fdatasync` fails | writer records the first error, keeps draining so nothing deadlocks, `append` and `close` return it |
| Slow disk or compression | in-flight limit reached | back-pressure to rings, then per-stream overflow policy, all losses counted |
| Sensor stall | stream goes quiet | `stream_stale` event after max(10 periods, 100 ms), `stream_resumed` with the gap length |
| Clock regression | timestamps go backwards | `ts_regression` event; seek and merge stay correct (running maximum) |
| Unclean shutdown via signal | SIGINT/SIGTERM | `mr_record` stops producers, drains, closes cleanly |

## Error handling

Recoverable errors (I/O, corrupt or truncated data, bad configuration) are
returned as `Result<T>` / `Status`, a small `std::expected`-style type (the
code base stays on C++20). The library never throws for these. Resource
exhaustion from the standard library (`std::bad_alloc`, `std::system_error`
from thread creation) is not caught inside the library; each tool's `main`
catches it and exits non-zero. Invariants are checked with `assert`.

## Testing

* Unit tests for every component (93 GoogleTest cases), including a
  multi-threaded ring stress test per overflow policy, CRC known vectors,
  every-byte header corruption, truncation at every byte of the last two
  chunks, reader seek/merge against brute force, quality, trigger window and
  privacy tests.
* `test_crash_kill` spawns `mr_record`, waits until it reports 25 durable
  chunks over a pipe, sends SIGKILL, recovers, and checks that every reported
  chunk is back and only a torn tail (less than one chunk) was dropped.
* `scripts/e2e_smoke.sh`: record, SIGKILL, recover, inspect, replay with the
  real tools.
* libFuzzer target over all on-disk parsers, re-running each input with CRCs
  recomputed so mutations reach zstd and record parsing.
* All of the above under ASan+UBSan and TSan; see
  `bench/results/verification.txt`.

## Limitations and next steps

Measured or known, roughly in order of impact:

1. **Head-of-line blocking under overload.** One in-flight budget for all
   streams (see Back-pressure). Fix: per-stream or per-class budgets, or
   shed camera chunks first when the semaphore is exhausted.
2. **Replay p99.** `mr_replay` decompresses the next chunk on the replay
   thread, so the message after a chunk boundary is late by about the decode
   time (p99 around 2 ms at 1x). Fix: decode ahead on a helper thread.
3. **Record header overhead.** 24 bytes per record is half the size of an
   IMU sample. Delta-encoded timestamps and implicit stream id/sequence in
   single-stream chunks would remove most of it.
4. **Camera data is compressed with a general-purpose codec** (1.13x at zstd
   1, 1.5x at zstd 9 on the synthetic frames). Real systems store encoded
   video or use an image codec; a delta/PNG-style filter before zstd is a
   cheap middle ground.
5. **I/O path.** Buffered `write(2)` plus `fdatasync`. Next: `io_uring` for
   asynchronous submission, `O_DIRECT` with aligned chunk buffers to avoid
   double buffering in the page cache, and `fallocate` to reserve space.
6. **Two copies of every camera frame** (slot -> scratch -> chunk builder).
   The collector could append directly from the slot for streams that need
   no privacy filtering.
7. **Recovery stops at the first bad chunk.** A salvage mode could resync on
   the next valid chunk header after media corruption.
8. **Data.** The sensors are simulated. A KITTI OXTS replay source exists;
   camera images and lidar from a real dataset, and privacy masking driven by
   a detector (GPU) instead of fixed ROIs, are natural extensions.
