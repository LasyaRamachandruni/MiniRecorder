#include <mr/reader.hpp>

#include <algorithm>
#include <bit>
#include <string>

namespace mr {

Result<Reader> Reader::open(const std::filesystem::path& path) {
  auto file = MappedFile::open(path);
  if (!file) {
    return std::move(file).error();
  }
  Reader reader;
  reader.file_ = std::move(file).value();
  const ByteSpan bytes = reader.file_.bytes();

  auto header = format::decode_file_header(bytes);
  if (!header) {
    return std::move(header).error();
  }
  reader.header_ = std::move(header).value();

  auto footer = format::read_footer(bytes);
  if (!footer) {
    Error e = std::move(footer).error();
    if (e.code == Errc::kNotFound) {
      e.message += "; run mr_recover on it first";
    }
    return e;
  }
  reader.footer_ = std::move(footer).value();

  // Validate the index: chunks must follow the header, be in file order and
  // must not overlap. read_footer() already bounded them by the footer offset.
  std::uint64_t min_offset = reader.header_.encoded_size;
  for (std::size_t i = 0; i < reader.footer_.chunks.size(); ++i) {
    const format::ChunkIndexEntry& c = reader.footer_.chunks[i];
    if (c.offset < min_offset) {
      return make_error(Errc::kCorrupt, "chunk index not in file order at entry " + std::to_string(i));
    }
    min_offset = c.offset + c.total_size;
    for (std::uint64_t mask = c.stream_mask; mask != 0; mask &= mask - 1) {
      const auto id = static_cast<std::size_t>(std::countr_zero(mask));
      const std::int64_t prev_max =
          reader.running_max_end_[id].empty() ? c.max_time_ns : reader.running_max_end_[id].back();
      reader.stream_chunks_[id].push_back(i);
      reader.running_max_end_[id].push_back(std::max(prev_max, c.max_time_ns));
    }
  }
  return reader;
}

const format::StreamInfo* Reader::stream_info(StreamId id) const noexcept {
  const auto it = std::find_if(header_.streams.begin(), header_.streams.end(),
                               [id](const format::StreamInfo& s) { return s.id == id; });
  return it == header_.streams.end() ? nullptr : &*it;
}

const format::StreamStats* Reader::stream_stats(StreamId id) const noexcept {
  const auto it = std::find_if(footer_.streams.begin(), footer_.streams.end(),
                               [id](const format::StreamStats& s) { return s.id == id; });
  return it == footer_.streams.end() ? nullptr : &*it;
}

std::span<const std::size_t> Reader::chunks_for_stream(StreamId stream) const noexcept {
  if (stream >= kMaxStreams) {
    return {};
  }
  return stream_chunks_[stream];
}

std::size_t Reader::seek_chunk(StreamId stream, std::int64_t t) const noexcept {
  if (stream >= kMaxStreams) {
    return 0;
  }
  const std::vector<std::int64_t>& ends = running_max_end_[stream];
  return static_cast<std::size_t>(std::lower_bound(ends.begin(), ends.end(), t) - ends.begin());
}

Status Reader::read_chunk(std::size_t chunk_index, ZstdDecompressor& decompressor,
                          std::vector<std::byte>& out) const {
  if (chunk_index >= footer_.chunks.size()) {
    return make_error(Errc::kInvalidArgument, "chunk index out of range");
  }
  const format::ChunkIndexEntry& entry = footer_.chunks[chunk_index];
  const ByteSpan chunk = file_.bytes().subspan(entry.offset, entry.total_size);
  auto header = format::decode_chunk_header(chunk);
  if (!header) {
    return std::move(header).error();
  }
  if (header->total_size() != entry.total_size || header->stream_mask != entry.stream_mask) {
    return make_error(Errc::kCorrupt, "chunk header disagrees with footer index");
  }
  return decode_chunk_body(header.value(), chunk.subspan(format::kChunkHeaderSize), decompressor, out);
}

MessageIterator Reader::messages(const ReadOptions& options) const {
  return MessageIterator(*this, options);
}

// ---------------------------------------------------------------- iterator

MessageIterator::MessageIterator(const Reader& reader, ReadOptions options)
    : reader_(&reader), options_(options) {
  for (const format::StreamInfo& s : reader.header().streams) {
    if ((options_.stream_mask & (std::uint64_t{1} << s.id)) == 0 ||
        reader.chunks_for_stream(s.id).empty()) {
      continue;
    }
    Cursor c;
    c.stream = s.id;
    c.next_chunk = reader.seek_chunk(s.id, options_.start_ns);
    cursors_.push_back(std::move(c));
  }
}

Result<bool> MessageIterator::advance(Cursor& c) {
  const std::span<const std::size_t> chunks = reader_->chunks_for_stream(c.stream);
  for (;;) {
    auto more = c.parser.next(c.current);
    if (!more) {
      return std::move(more).error();
    }
    if (more.value()) {
      const std::int64_t t = c.current.meta.timestamp_ns;
      if (c.current.meta.stream_id == c.stream && t >= options_.start_ns && t <= options_.end_ns) {
        return true;
      }
      continue;
    }
    // Current chunk exhausted: load the next one that overlaps the range.
    bool loaded = false;
    while (c.next_chunk < chunks.size()) {
      const std::size_t idx = chunks[c.next_chunk++];
      const format::ChunkIndexEntry& e = reader_->footer().chunks[idx];
      if (e.max_time_ns < options_.start_ns || e.min_time_ns > options_.end_ns) {
        continue;
      }
      MR_RETURN_IF_ERROR(reader_->read_chunk(idx, decompressor_, c.body));
      c.parser = format::RecordParser(c.body);
      loaded = true;
      break;
    }
    if (!loaded) {
      return false;
    }
  }
}

bool MessageIterator::later(std::size_t a, std::size_t b) const noexcept {
  // std heap algorithms build max-heaps, so "less" means "comes later" to get
  // the earliest (timestamp, stream id) at the top.
  const RecordMeta& ma = cursors_[a].current.meta;
  const RecordMeta& mb = cursors_[b].current.meta;
  return ma.timestamp_ns != mb.timestamp_ns ? ma.timestamp_ns > mb.timestamp_ns
                                            : ma.stream_id > mb.stream_id;
}

void MessageIterator::push_heap_entry(std::size_t cursor_index) {
  heap_.push_back(cursor_index);
  std::push_heap(heap_.begin(), heap_.end(), [this](std::size_t a, std::size_t b) { return later(a, b); });
}

Status MessageIterator::prime() {
  primed_ = true;
  for (std::size_t i = 0; i < cursors_.size(); ++i) {
    auto has = advance(cursors_[i]);
    if (!has) {
      return std::move(has).error();
    }
    if (has.value()) {
      push_heap_entry(i);
    }
  }
  return ok_status();
}

Result<bool> MessageIterator::next(Message& out) {
  if (!primed_) {
    MR_RETURN_IF_ERROR(prime());
  }
  // Advance the cursor that produced the previous message only now, so the
  // payload span handed out last time stayed valid until this call.
  if (pending_advance_) {
    const std::size_t i = *pending_advance_;
    pending_advance_.reset();
    auto has = advance(cursors_[i]);
    if (!has) {
      return std::move(has).error();
    }
    if (has.value()) {
      push_heap_entry(i);
    }
  }
  if (heap_.empty()) {
    return false;
  }
  std::pop_heap(heap_.begin(), heap_.end(), [this](std::size_t a, std::size_t b) { return later(a, b); });
  const std::size_t i = heap_.back();
  heap_.pop_back();
  out.meta = cursors_[i].current.meta;
  out.payload = cursors_[i].current.payload;
  pending_advance_ = i;
  return true;
}

}  // namespace mr
