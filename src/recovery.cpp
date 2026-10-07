#include <algorithm>
#include <array>
#include <bit>
#include <fcntl.h>
#include <format>
#include <vector>

#include <mr/chunk.hpp>
#include <mr/crc32c.hpp>
#include <mr/events.hpp>
#include <mr/file_util.hpp>
#include <mr/format.hpp>
#include <mr/recovery.hpp>

namespace mr {
namespace {

// Returns true if the file already ends with a valid trailer + footer.
bool has_valid_footer(int fd, std::uint64_t size) {
  if (size < format::kTrailerSize) {
    return false;
  }
  std::array<std::byte, format::kTrailerSize> tail{};
  if (!pread_exact(fd, tail, size - format::kTrailerSize)) {
    return false;
  }
  auto trailer = format::decode_trailer(tail);
  if (!trailer) {
    return false;
  }
  const format::Trailer& t = trailer.value();
  if (t.footer_offset > size || size - t.footer_offset < format::kTrailerSize ||
      size - t.footer_offset - format::kTrailerSize != t.footer_size ||
      t.footer_size > format::kMaxChunkBytes) {
    return false;
  }
  std::vector<std::byte> footer(t.footer_size);
  if (!pread_exact(fd, footer, t.footer_offset)) {
    return false;
  }
  return static_cast<bool>(format::decode_footer(footer, t));
}

struct StreamAccumulator {
  format::StreamStats stats;
  bool seen = false;

  void add(std::int64_t t, std::size_t payload_size) { add_range(t, t, 1, payload_size); }

  void add_range(std::int64_t min_t, std::int64_t max_t, std::uint64_t records,
                 std::uint64_t payload_bytes) {
    if (!seen) {
      stats.first_time_ns = min_t;
      stats.last_time_ns = max_t;
      seen = true;
    }
    stats.first_time_ns = std::min(stats.first_time_ns, min_t);
    stats.last_time_ns = std::max(stats.last_time_ns, max_t);
    stats.records += records;
    stats.payload_bytes += payload_bytes;
  }
};

}  // namespace

std::filesystem::path default_recovery_output(const std::filesystem::path& input) {
  const std::string s = input.string();
  constexpr std::string_view kSuffix = ".partial";
  if (s.size() > kSuffix.size() && s.ends_with(kSuffix)) {
    return {s.substr(0, s.size() - kSuffix.size())};
  }
  return input;
}

Result<RecoveryReport> recover_file(const std::filesystem::path& input,
                                    const std::filesystem::path& output,
                                    const RecoveryOptions& options) {
  auto fd_result = open_file(input, options.dry_run ? O_RDONLY : O_RDWR);
  if (!fd_result) {
    return std::move(fd_result).error();
  }
  FileDescriptor fd = std::move(fd_result).value();
  auto size_result = file_size(fd.get());
  if (!size_result) {
    return std::move(size_result).error();
  }
  const std::uint64_t size = size_result.value();

  RecoveryReport report;
  report.output = output;
  report.input_bytes = size;

  // ---- file header: without it nothing can be interpreted ------------------
  std::vector<std::byte> buf(std::min<std::uint64_t>(size, format::kFileHeaderFixedSize));
  MR_RETURN_IF_ERROR(pread_exact(fd.get(), buf, 0));
  if (buf.size() < format::kFileHeaderFixedSize) {
    return make_error(Errc::kTruncated, "file shorter than a file header; nothing to recover");
  }
  const auto header_size = load_le<std::uint32_t>(buf.data() + 12);
  if (header_size > size || header_size > 1U << 20) {
    return make_error(Errc::kCorrupt, "file header size field is invalid");
  }
  buf.resize(header_size);
  MR_RETURN_IF_ERROR(pread_exact(fd.get(), buf, 0));
  auto header = format::decode_file_header(buf);
  if (!header) {
    return std::move(header).error();
  }

  if (has_valid_footer(fd.get(), size)) {
    report.already_complete = true;
    report.bytes_kept = size;
    report.stop_reason = "file already has a valid footer";
    if (!options.dry_run && output != input) {
      MR_RETURN_IF_ERROR(fd.close());
      MR_RETURN_IF_ERROR(durable_rename(input, output));
    }
    return report;
  }

  // ---- scan chunks -----------------------------------------------------------
  std::array<StreamAccumulator, kMaxStreams> acc{};
  std::array<StreamQualityCounters, kMaxStreams> quality{};
  std::vector<format::ChunkIndexEntry> index;
  ZstdDecompressor decompressor;
  std::vector<std::byte> chunk;
  std::vector<std::byte> body;
  std::uint64_t offset = header->encoded_size;
  std::uint64_t expected_seq = 0;

  for (;;) {
    const std::uint64_t remaining = size - offset;
    if (remaining == 0) {
      report.stop_reason = "clean end of data (no footer)";
      break;
    }
    if (remaining < format::kChunkHeaderSize) {
      report.stop_reason = std::format("torn chunk header at offset {}", offset);
      break;
    }
    chunk.resize(format::kChunkHeaderSize);
    MR_RETURN_IF_ERROR(pread_exact(fd.get(), chunk, offset));
    auto h = format::decode_chunk_header(chunk);
    if (!h) {
      report.stop_reason = std::format("{} at offset {}", h.error().message, offset);
      break;
    }
    if (h->chunk_seq != expected_seq) {
      report.stop_reason = std::format("chunk sequence {} where {} was expected at offset {}",
                                       h->chunk_seq, expected_seq, offset);
      break;
    }
    if (remaining - format::kChunkHeaderSize < h->compressed_size) {
      report.stop_reason = std::format("torn chunk body at offset {}", offset);
      break;
    }
    chunk.resize(h->total_size());
    MR_RETURN_IF_ERROR(pread_exact(fd.get(), std::span(chunk).subspan(format::kChunkHeaderSize),
                                   offset + format::kChunkHeaderSize));

    if (options.verify_records) {
      auto verified = parse_and_verify_chunk(chunk, decompressor, body);
      if (!verified) {
        report.stop_reason =
            std::format("{} in chunk at offset {}", verified.error().message, offset);
        break;
      }
      format::RecordParser parser(body);
      format::RecordView rec;
      while (parser.next(rec).value()) {  // already validated above
        acc[rec.meta.stream_id].add(rec.meta.timestamp_ns, rec.payload.size());
        if (rec.meta.stream_id == kEventStreamId) {
          if (const auto ev = decode_event(rec.payload)) {
            accumulate_event(*ev, quality);
          }
        }
      }
    } else {
      const ByteSpan payload = std::span<const std::byte>(chunk).subspan(format::kChunkHeaderSize);
      if (crc32c::value(payload) != h->payload_crc) {
        report.stop_reason = std::format("chunk body CRC mismatch at offset {}", offset);
        break;
      }
      // Without decompression, attribute single-stream chunks from the header.
      if (std::popcount(h->stream_mask) == 1) {
        // decode_chunk_header() guarantees record_count * 24 <= uncompressed_size.
        acc[static_cast<std::size_t>(std::countr_zero(h->stream_mask))].add_range(
            h->min_time_ns, h->max_time_ns, h->record_count,
            h->uncompressed_size - std::uint64_t{h->record_count} * format::kRecordHeaderSize);
      }
    }

    if (std::popcount(h->stream_mask) == 1) {
      acc[static_cast<std::size_t>(std::countr_zero(h->stream_mask))].stats.stored_bytes +=
          h->total_size();
    }
    index.push_back({.offset = offset,
                     .stream_mask = h->stream_mask,
                     .min_time_ns = h->min_time_ns,
                     .max_time_ns = h->max_time_ns,
                     .record_count = h->record_count,
                     .total_size = static_cast<std::uint32_t>(h->total_size()),
                     .uncompressed_size = h->uncompressed_size});
    report.records_recovered += h->record_count;
    offset += h->total_size();
    ++expected_seq;
  }

  report.chunks_recovered = index.size();
  report.bytes_kept = offset;
  report.bytes_discarded = size - offset;
  if (options.dry_run) {
    return report;
  }

  // ---- rebuild footer ----------------------------------------------------------
  format::Footer footer;
  footer.flags = format::footer_flags::kRecovered;
  footer.chunks = std::move(index);
  bool any = false;
  for (const format::StreamInfo& info : header->streams) {
    format::StreamStats st = acc[info.id].stats;
    st.id = info.id;
    const StreamQualityCounters& q = quality[info.id];
    st.dropped = q.dropped;
    st.seq_gaps = q.seq_gaps;
    st.ts_regressions = q.ts_regressions;
    st.rate_events = q.rate_events;
    st.stale_events = q.stale_events;
    if (st.records > 0) {
      footer.start_time_ns =
          any ? std::min(footer.start_time_ns, st.first_time_ns) : st.first_time_ns;
      footer.end_time_ns = any ? std::max(footer.end_time_ns, st.last_time_ns) : st.last_time_ns;
      any = true;
    }
    footer.streams.push_back(st);
  }

  MR_RETURN_IF_ERROR(truncate_file(fd.get(), offset));
  MR_RETURN_IF_ERROR(pwrite_all(fd.get(), format::encode_footer(footer, offset), offset));
  MR_RETURN_IF_ERROR(sync_data(fd.get()));
  MR_RETURN_IF_ERROR(fd.close());
  if (output != input) {
    MR_RETURN_IF_ERROR(durable_rename(input, output));
  }
  return report;
}

}  // namespace mr
