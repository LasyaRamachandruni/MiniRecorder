#include <array>
#include <cstdint>
#include <random>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <mr/bytes.hpp>
#include <mr/crc32c.hpp>

namespace {

using mr::crc32c::extend_hw;
using mr::crc32c::extend_portable;

std::vector<std::byte> filled(std::size_t n, std::uint8_t value) {
  return std::vector<std::byte>(n, std::byte{value});
}

// Known answers: "123456789" is the standard check value, the 32-byte
// vectors are from RFC 3720 (iSCSI), appendix B.4.
struct Vector {
  std::vector<std::byte> data;
  std::uint32_t expected;
};

std::vector<Vector> known_vectors() {
  std::vector<Vector> out;
  const auto check = mr::as_bytes("123456789");
  out.push_back({{check.begin(), check.end()}, 0xE3069283U});
  out.push_back({filled(32, 0x00), 0x8A9136AAU});
  out.push_back({filled(32, 0xFF), 0x62A8AB43U});
  std::vector<std::byte> ascending(32);
  std::vector<std::byte> descending(32);
  for (std::size_t i = 0; i < 32; ++i) {
    ascending[i] = static_cast<std::byte>(i);
    descending[i] = static_cast<std::byte>(31 - i);
  }
  out.push_back({ascending, 0x46DD794EU});
  out.push_back({descending, 0x113FDB5CU});
  out.push_back({{}, 0x00000000U});
  return out;
}

TEST(Crc32c, PortableMatchesKnownVectors) {
  for (const auto& v : known_vectors()) {
    EXPECT_EQ(extend_portable(0, v.data), v.expected) << "size " << v.data.size();
  }
}

TEST(Crc32c, HardwareMatchesKnownVectors) {
  if (!mr::crc32c::hw_available()) {
    GTEST_SKIP() << "SSE4.2 not available";
  }
  for (const auto& v : known_vectors()) {
    EXPECT_EQ(extend_hw(0, v.data), v.expected) << "size " << v.data.size();
  }
}

TEST(Crc32c, ExtendIsIncremental) {
  std::mt19937_64 rng(42);
  std::vector<std::byte> data(4099);
  for (auto& b : data) {
    b = static_cast<std::byte>(rng() & 0xFFU);
  }
  const std::uint32_t whole = mr::crc32c::value(data);
  const std::span<const std::byte> all(data);
  for (const std::size_t split : {0UL, 1UL, 7UL, 8UL, 9UL, 1000UL, 4098UL, 4099UL}) {
    const std::uint32_t a = mr::crc32c::extend(0, all.first(split));
    EXPECT_EQ(mr::crc32c::extend(a, all.subspan(split)), whole) << "split " << split;
  }
}

TEST(Crc32c, PortableAndHardwareAgreeOnRandomInput) {
  if (!mr::crc32c::hw_available()) {
    GTEST_SKIP() << "SSE4.2 not available";
  }
  std::mt19937_64 rng(7);
  for (int round = 0; round < 200; ++round) {
    std::vector<std::byte> data(rng() % 2048);
    for (auto& b : data) {
      b = static_cast<std::byte>(rng() & 0xFFU);
    }
    // Misaligned views exercise the head/tail handling of both versions.
    const std::size_t offset = data.empty() ? 0 : rng() % std::min<std::size_t>(data.size(), 8);
    const std::span<const std::byte> view = std::span<const std::byte>(data).subspan(offset);
    EXPECT_EQ(extend_portable(0, view), extend_hw(0, view));
  }
}

TEST(Crc32c, DetectsSingleBitFlip) {
  std::vector<std::byte> data = filled(1024, 0x5A);
  const std::uint32_t before = mr::crc32c::value(data);
  data[517] ^= std::byte{0x04};
  EXPECT_NE(mr::crc32c::value(data), before);
}

}  // namespace
