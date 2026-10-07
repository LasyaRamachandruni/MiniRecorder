# MiniRecorder log format, version 1

A log (`*.mrlog`) is a single append-only file. While it is being written it
is named `*.mrlog.partial`; it is renamed to its final name only after the
footer has been written and synced. All integers are little-endian, all
structures are packed (no padding), and every structure carries a CRC32C
(Castagnoli polynomial, reflected 0x82F63B78, initial value and final XOR
0xFFFFFFFF; check value of `"123456789"` is `0xE3069283`).

```
+-------------+---------+---------+-----+---------+--------+---------+
| File header | Chunk 0 | Chunk 1 | ... | Chunk N | Footer | Trailer |
+-------------+---------+---------+-----+---------+--------+---------+
                \______ written in chunk_seq order ______/
```

The source of truth for every constant below is `include/mr/format.hpp`.

## File header

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | bytes | magic `4D 52 4C 4F 47 0D 0A 1A` (`"MRLOG\r\n\x1A"`) |
| 8 | 2 | u16 | format version (1) |
| 10 | 2 | u16 | stream count `n` (at most 64) |
| 12 | 4 | u32 | header size in bytes = `32 + 48 * n + 4` |
| 16 | 8 | i64 | creation time, Unix nanoseconds |
| 24 | 4 | u32 | flags (0) |
| 28 | 4 | u32 | reserved (0) |
| 32 | 48 * n | | stream table, below |
| 32 + 48n | 4 | u32 | CRC32C of all preceding header bytes |

The magic includes CR, LF and Ctrl-Z, like PNG's, so a file mangled by a
text-mode transfer is rejected immediately.

Stream table entry (48 bytes):

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 2 | u16 | stream id (0..63, unique) |
| 2 | 2 | u16 | sensor kind: 1 IMU, 2 GPS, 3 wheel/CAN, 4 camera, 5 events |
| 4 | 4 | u32 | nominal rate in millihertz (0 = not periodic) |
| 8 | 40 | char | name, NUL padded, not necessarily NUL terminated |

Stream id 63 is reserved for the quality/trigger event stream (`_events`).

The writer syncs the header (and the directory entry of the `.partial` file)
before writing any chunk, so a crash leftover always has a valid header.

## Chunk

A chunk is a 64-byte header followed by `compressed_size` bytes of body.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | u32 | magic `0x4B43524D` (`"MRCK"` on disk) |
| 4 | 2 | u16 | version (1) |
| 6 | 2 | u16 | header size (64) |
| 8 | 4 | u32 | compression: 0 none, 1 zstd |
| 12 | 4 | u32 | record count |
| 16 | 8 | u64 | stream mask: bit `i` set if stream `i` has records in the chunk |
| 24 | 8 | i64 | minimum record timestamp (ns) |
| 32 | 8 | i64 | maximum record timestamp (ns) |
| 40 | 4 | u32 | uncompressed body size |
| 44 | 4 | u32 | compressed body size (bytes following this header) |
| 48 | 8 | u64 | chunk sequence number: 0, 1, 2, ... in file order |
| 56 | 4 | u32 | CRC32C of the body bytes as stored (after compression) |
| 60 | 4 | u32 | CRC32C of header bytes 0..59 |

Body: the concatenated records, either raw (compression 0, in which case both
sizes are equal) or as one zstd frame. The writer stores a chunk raw when
zstd does not make it smaller.

The current writer only produces single-stream chunks (exactly one bit set
in the mask). The format and the reader allow several streams per chunk.

### Record

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | i64 | sensor timestamp, ns on the vehicle monotonic clock |
| 8 | 8 | u64 | per-stream sequence number |
| 16 | 2 | u16 | stream id |
| 18 | 2 | u16 | flags: bit 0 privacy-filtered, bit 1 pre-trigger, bit 2 injected fault |
| 20 | 4 | u32 | payload size |
| 24 | n | bytes | payload |

## Footer

Written once, after the last chunk, when the file is closed (or rebuilt by
recovery).

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 4 | u32 | magic `0x5446524D` (`"MRFT"` on disk) |
| 4 | 2 | u16 | version (1) |
| 6 | 2 | u16 | flags: bit 0 = rebuilt by recovery |
| 8 | 4 | u32 | chunk count `c` |
| 12 | 4 | u32 | stream count `s` |
| 16 | 8 | i64 | earliest record timestamp in the file |
| 24 | 8 | i64 | latest record timestamp in the file |
| 32 | 48 * c | | chunk index |
| 32 + 48c | 88 * s | | per-stream statistics |

Chunk index entry (48 bytes), in file order:

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | u64 | file offset of the chunk header |
| 8 | 8 | u64 | stream mask |
| 16 | 8 | i64 | minimum timestamp |
| 24 | 8 | i64 | maximum timestamp |
| 32 | 4 | u32 | record count |
| 36 | 4 | u32 | total size (64 + compressed body size) |
| 40 | 4 | u32 | uncompressed body size |
| 44 | 4 | u32 | reserved |

Per-stream statistics (88 bytes):

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 2 | u16 | stream id |
| 2 | 6 | | reserved |
| 8 | 8 | u64 | records |
| 16 | 8 | u64 | payload bytes |
| 24 | 8 | u64 | stored bytes (chunks containing only this stream) |
| 32 | 8 | i64 | earliest timestamp |
| 40 | 8 | i64 | latest timestamp |
| 48 | 8 | u64 | records dropped by the ring overflow policy |
| 56 | 8 | u64 | sequence-gap events |
| 64 | 8 | u64 | timestamp-regression events |
| 72 | 8 | u64 | rate-out-of-bounds events |
| 80 | 8 | u64 | stale-stream events |

## Trailer

The last 24 bytes of a complete file.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 8 | u64 | footer offset |
| 8 | 4 | u32 | footer size |
| 12 | 4 | u32 | CRC32C of the footer bytes |
| 16 | 8 | bytes | magic `"MRLOGEND"` |

A reader opens a file by reading the trailer, checking that
`footer_offset + footer_size + 24 == file size`, verifying the footer CRC,
and checking that every index entry lies between the header and the footer,
in increasing offset order, without overlap.

## Event payload (stream 63)

32 bytes: `u16 stream`, `u16 kind`, `u32 reserved`, `i64 timestamp_ns`,
`i64 value`, `i64 expected`. Kinds: 1 sequence gap, 2 timestamp regression,
3 rate out of bounds, 4 rate recovered, 5 stream stale, 6 stream resumed,
7 trigger, 8 records dropped. See `include/mr/events.hpp` for the meaning of
`value`/`expected` per kind. Because all footer quality counters are derived
from these events, recovery can rebuild them.

## Sensor payloads (simulator)

| Kind | Size | Layout |
|---|---:|---|
| IMU | 48 | 6 x f64: accel x/y/z (m/s^2), gyro x/y/z (rad/s) |
| GPS | 40 | f64 lat, f64 lon (deg), f64 alt (m), f32 speed (m/s), f32 heading (deg), u8 fix, u8 sats, 6 reserved |
| Wheel/CAN | 16 | u32 CAN id (0x1A0), u8 DLC, 3 pad, 4 x u16 wheel speed (0.01 km/h) |
| Camera | 16 + W*H | u16 W, u16 H, u8 format (0 = GRAY8), 3 pad, u64 frame id, pixels |

## Validation rules

Every parser is bounds checked and treats sizes as untrusted:

* chunk and record sizes are capped (64 MiB per chunk body, 16 MiB per payload)
  before any allocation;
* `record_count * 24 <= uncompressed_size`, `min_time <= max_time`;
* a raw chunk must have equal compressed and uncompressed sizes;
* decompression must produce exactly `uncompressed_size` bytes;
* the records must exactly fill the body, their count must match the header,
  their timestamps must lie in the header's range and their streams must be
  in the stream mask.

These parsers are fuzzed by `fuzz/fuzz_chunk.cpp`.

## Recovery

A file without a valid trailer is recovered by scanning from the end of the
header: read a chunk header, check magic, header CRC, version, sequence
number (`expected_seq`, starting at 0), that the body fits in the file, the
body CRC and (by default) decompression and record validity. The first chunk
that fails ends the scan; the file is truncated there, a footer with the
"recovered" flag is appended and synced, and the file is renamed. Per-stream
record counts, sizes and time ranges are recomputed from the chunks; quality
and drop counters are recomputed from the event stream.

The writer appends whole chunks strictly in sequence order and never rewrites
data. After a *process* crash every completed `write(2)` is in the page cache,
so only the chunk being written can be incomplete and recovery keeps
everything before it. After a *power loss*, data written since the last
completed `fdatasync` may survive partially and in any order; recovery keeps
the longest valid prefix, which always contains every chunk covered by a
completed `fdatasync` (assuming the device honours cache flushes). The
sequence-number check additionally rejects a valid-looking chunk that is out
of place, e.g. a duplicated block.

## Compatibility

Readers reject versions other than 1. Reserved fields are written as zero and
ignored when reading. New chunk compression codes or flags require a version
bump.
