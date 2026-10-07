#include <array>

#include <mr/bytes.hpp>
#include <mr/crc32c.hpp>

#if defined(__x86_64__)
#include <nmmintrin.h>
#endif

namespace mr::crc32c {
namespace {

constexpr std::uint32_t kPolyReflected = 0x82F63B78U;

using Table = std::array<std::array<std::uint32_t, 256>, 8>;

// Slice-by-8 tables: table[0] is the classic byte-at-a-time table and
// table[k][i] is the CRC of byte i followed by k zero bytes.
consteval Table make_tables() {
  Table t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0 ? (crc >> 1) ^ kPolyReflected : crc >> 1;
    }
    t[0][i] = crc;
  }
  for (std::size_t k = 1; k < 8; ++k) {
    for (std::size_t i = 0; i < 256; ++i) {
      const std::uint32_t prev = t[k - 1][i];
      t[k][i] = (prev >> 8) ^ t[0][prev & 0xFFU];
    }
  }
  return t;
}

constexpr Table kTables = make_tables();
static_assert(kTables[0][1] == 0xF26B8303U, "CRC32C table generation is wrong");

inline std::uint32_t byte_step(std::uint32_t crc, std::byte b) noexcept {
  return kTables[0][(crc ^ std::to_integer<std::uint32_t>(b)) & 0xFFU] ^ (crc >> 8);
}

}  // namespace

std::uint32_t extend_portable(std::uint32_t crc, std::span<const std::byte> data) noexcept {
  std::uint32_t l = ~crc;
  std::size_t i = 0;
  const std::size_t n = data.size();

  for (; i + 8 <= n; i += 8) {
    const auto lo = load_le<std::uint32_t>(&data[i]) ^ l;
    const auto hi = load_le<std::uint32_t>(&data[i + 4]);
    l = kTables[7][lo & 0xFFU] ^ kTables[6][(lo >> 8) & 0xFFU] ^ kTables[5][(lo >> 16) & 0xFFU] ^
        kTables[4][lo >> 24] ^ kTables[3][hi & 0xFFU] ^ kTables[2][(hi >> 8) & 0xFFU] ^
        kTables[1][(hi >> 16) & 0xFFU] ^ kTables[0][hi >> 24];
  }
  for (; i < n; ++i) {
    l = byte_step(l, data[i]);
  }
  return ~l;
}

#if defined(__x86_64__)

__attribute__((target("sse4.2"))) std::uint32_t extend_hw(
    std::uint32_t crc, std::span<const std::byte> data) noexcept {
  std::uint64_t l = ~crc;
  std::size_t i = 0;
  const std::size_t n = data.size();
  for (; i + 8 <= n; i += 8) {
    l = _mm_crc32_u64(l, load_le<std::uint64_t>(&data[i]));
  }
  auto l32 = static_cast<std::uint32_t>(l);
  for (; i < n; ++i) {
    l32 = _mm_crc32_u8(l32, std::to_integer<std::uint8_t>(data[i]));
  }
  return ~l32;
}

bool hw_available() noexcept {
  static const bool available =
      __builtin_cpu_supports("sse4.2") != 0;  // NOLINT(readability-implicit-bool-conversion)
  return available;
}

#else

std::uint32_t extend_hw(std::uint32_t crc, std::span<const std::byte> data) noexcept {
  return extend_portable(crc, data);
}

bool hw_available() noexcept {
  return false;
}

#endif

std::uint32_t extend(std::uint32_t crc, std::span<const std::byte> data) noexcept {
  return hw_available() ? extend_hw(crc, data) : extend_portable(crc, data);
}

}  // namespace mr::crc32c
