// libFuzzer target for every parser that reads untrusted bytes from disk:
// chunk header + body (decompression, record framing), the record parser,
// the footer/trailer and the file header.
//
// CRCs would stop the fuzzer at the first check, so each input is also run
// with its CRC fields recomputed ("fixed up"). That lets mutations reach the
// zstd and record parsing code without weakening the production checks.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <mr/chunk.hpp>
#include <mr/crc32c.hpp>
#include <mr/events.hpp>
#include <mr/format.hpp>

namespace {

namespace fmt = mr::format;

void fix_chunk_crcs(std::vector<std::byte>& buf) {
  if (buf.size() < fmt::kChunkHeaderSize) {
    return;
  }
  const auto compressed = mr::load_le<std::uint32_t>(buf.data() + 44);
  const std::size_t body = std::min<std::size_t>(compressed, buf.size() - fmt::kChunkHeaderSize);
  mr::store_le<std::uint32_t>(buf.data() + 56,
                              mr::crc32c::value(mr::ByteSpan(buf).subspan(fmt::kChunkHeaderSize, body)));
  mr::store_le<std::uint32_t>(buf.data() + 60, mr::crc32c::value(mr::ByteSpan(buf).first(60)));
}

void fix_footer_crc(std::vector<std::byte>& buf) {
  if (buf.size() < fmt::kTrailerSize) {
    return;
  }
  std::byte* t = buf.data() + buf.size() - fmt::kTrailerSize;
  const auto offset = mr::load_le<std::uint64_t>(t);
  const auto size = mr::load_le<std::uint32_t>(t + 8);
  if (offset <= buf.size() && size <= buf.size() - offset) {
    mr::store_le<std::uint32_t>(t + 12, mr::crc32c::value(mr::ByteSpan(buf).subspan(offset, size)));
  }
}

void exercise(mr::ByteSpan in) {
  static mr::ZstdDecompressor decompressor;
  static std::vector<std::byte> out;

  (void)mr::parse_and_verify_chunk(in, decompressor, out);

  mr::format::RecordParser parser(in);
  mr::format::RecordView rec;
  for (int i = 0; i < 100000; ++i) {
    auto more = parser.next(rec);
    if (!more || !more.value()) {
      break;
    }
    (void)mr::decode_event(rec.payload);
  }

  (void)fmt::read_footer(in);
  (void)fmt::decode_file_header(in);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  std::vector<std::byte> buf(size);
  if (size > 0) {
    std::memcpy(buf.data(), data, size);
  }
  exercise(buf);
  fix_chunk_crcs(buf);
  exercise(buf);
  fix_footer_crc(buf);
  exercise(buf);
  return 0;
}
