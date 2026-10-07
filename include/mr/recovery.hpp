#pragma once

// Crash recovery for files left as ".partial" (no footer).
//
// Because the writer appends complete chunks strictly in order, a crash can
// only damage the tail of the file. Recovery scans chunks from the front,
// validating header CRC, sequence number, bounds and body CRC (and, by
// default, decompressing and checking every record). It stops at the first
// chunk that fails, truncates the file there, appends a rebuilt footer
// (marked RECOVERED), syncs, and renames it to the output name.
//
// Contract: every chunk that was completely written before the crash and is
// not corrupted is kept. Data after the first bad chunk is discarded even if
// later chunks are intact; with a single ordered writer that only happens
// with media corruption, not with a crash.

#include <cstdint>
#include <filesystem>
#include <string>

#include <mr/result.hpp>

namespace mr {

struct RecoveryOptions {
  // Decompress chunks and validate every record (slower, stronger). When
  // false, only header/body CRCs are checked and per-stream statistics are
  // derived from chunk headers.
  bool verify_records = true;
  // Scan and report without modifying anything.
  bool dry_run = false;
};

struct RecoveryReport {
  std::filesystem::path output;
  bool already_complete = false;  // input had a valid footer; only renamed
  std::uint64_t chunks_recovered = 0;
  std::uint64_t records_recovered = 0;
  std::uint64_t input_bytes = 0;
  std::uint64_t bytes_kept = 0;       // header + valid chunks
  std::uint64_t bytes_discarded = 0;  // torn/corrupt tail
  std::string stop_reason;
};

// `output` may equal `input`. If it differs, the input name is gone afterwards
// (the file is renamed, never copied).
[[nodiscard]] Result<RecoveryReport> recover_file(const std::filesystem::path& input,
                                                  const std::filesystem::path& output,
                                                  const RecoveryOptions& options = {});

// Default output name: strips a trailing ".partial".
[[nodiscard]] std::filesystem::path default_recovery_output(const std::filesystem::path& input);

}  // namespace mr
