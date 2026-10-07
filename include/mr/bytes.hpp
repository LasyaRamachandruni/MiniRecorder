#pragma once

// Explicit little-endian encoding helpers. All on-disk integers go through
// these functions, so the file format does not depend on host endianness or
// struct layout, and there is no type punning through reinterpret_cast.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace mr {

using ByteSpan = std::span<const std::byte>;
using MutableByteSpan = std::span<std::byte>;

template <typename T>
concept LeInteger = std::is_integral_v<T> && !std::is_same_v<T, bool>;

template <LeInteger T>
[[nodiscard]] constexpr T load_le(const std::byte* src) noexcept {
  using U = std::make_unsigned_t<T>;
  U value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value = static_cast<U>(value | (static_cast<U>(std::to_integer<std::uint8_t>(src[i])) << (8 * i)));
  }
  return static_cast<T>(value);
}

template <LeInteger T>
constexpr void store_le(std::byte* dst, T value) noexcept {
  using U = std::make_unsigned_t<T>;
  const auto bits = static_cast<U>(value);
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    dst[i] = static_cast<std::byte>((bits >> (8 * i)) & 0xFFU);
  }
}

[[nodiscard]] inline double load_le_f64(const std::byte* src) noexcept {
  return std::bit_cast<double>(load_le<std::uint64_t>(src));
}
inline void store_le_f64(std::byte* dst, double value) noexcept {
  store_le<std::uint64_t>(dst, std::bit_cast<std::uint64_t>(value));
}
[[nodiscard]] inline float load_le_f32(const std::byte* src) noexcept {
  return std::bit_cast<float>(load_le<std::uint32_t>(src));
}
inline void store_le_f32(std::byte* dst, float value) noexcept {
  store_le<std::uint32_t>(dst, std::bit_cast<std::uint32_t>(value));
}

// Appends little-endian values to a growable buffer.
class ByteWriter {
 public:
  explicit ByteWriter(std::vector<std::byte>& out) noexcept : out_(&out) {}

  template <LeInteger T>
  void put(T value) {
    const std::size_t pos = out_->size();
    out_->resize(pos + sizeof(T));
    store_le<T>(out_->data() + pos, value);
  }
  void put_bytes(ByteSpan bytes) { out_->insert(out_->end(), bytes.begin(), bytes.end()); }
  void put_fixed_string(std::string_view text, std::size_t width) {
    const std::size_t pos = out_->size();
    out_->resize(pos + width, std::byte{0});
    std::memcpy(out_->data() + pos, text.data(), std::min(text.size(), width));
  }
  [[nodiscard]] std::size_t size() const noexcept { return out_->size(); }

 private:
  std::vector<std::byte>* out_;
};

// Bounds-checked little-endian reader. Once a read runs past the end the
// reader becomes "failed" and all further reads return zero; callers check
// ok() once at the end, which keeps parsing code linear and readable.
class ByteReader {
 public:
  explicit ByteReader(ByteSpan data) noexcept : data_(data) {}

  template <LeInteger T>
  [[nodiscard]] T get() noexcept {
    if (!require(sizeof(T))) {
      return T{0};
    }
    const T value = load_le<T>(data_.data() + pos_);
    pos_ += sizeof(T);
    return value;
  }
  [[nodiscard]] ByteSpan get_bytes(std::size_t count) noexcept {
    if (!require(count)) {
      return {};
    }
    const ByteSpan out = data_.subspan(pos_, count);
    pos_ += count;
    return out;
  }
  [[nodiscard]] std::string get_fixed_string(std::size_t width) {
    const ByteSpan raw = get_bytes(width);
    std::string out;
    for (const std::byte b : raw) {
      if (b == std::byte{0}) {
        break;
      }
      out.push_back(static_cast<char>(b));
    }
    return out;
  }
  void skip(std::size_t count) noexcept {
    if (require(count)) {
      pos_ += count;
    }
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::size_t position() const noexcept { return pos_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? data_.size() - pos_ : 0; }

 private:
  bool require(std::size_t count) noexcept {
    if (!ok_ || data_.size() - pos_ < count) {
      ok_ = false;
      return false;
    }
    return true;
  }

  ByteSpan data_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

[[nodiscard]] inline ByteSpan as_bytes(std::string_view text) noexcept {
  return std::as_bytes(std::span<const char>(text.data(), text.size()));
}

}  // namespace mr
