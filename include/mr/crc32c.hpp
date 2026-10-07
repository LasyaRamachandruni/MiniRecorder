#pragma once

// CRC-32C (Castagnoli, reflected polynomial 0x82F63B78), the checksum used by
// iSCSI, ext4 metadata and many storage engines. Two implementations:
//
//  * extend_portable: table-driven slice-by-8, tables generated at compile time.
//  * extend_hw: SSE4.2 CRC32 instruction (x86-64 only), selected at runtime.
//
// extend() dispatches to the fastest available implementation. All functions
// take and return the *finalized* CRC value, so extend(extend(0, a), b) equals
// value(a ++ b).

#include <cstddef>
#include <cstdint>
#include <span>

namespace mr::crc32c {

[[nodiscard]] std::uint32_t extend_portable(std::uint32_t crc, std::span<const std::byte> data) noexcept;
[[nodiscard]] std::uint32_t extend_hw(std::uint32_t crc, std::span<const std::byte> data) noexcept;
[[nodiscard]] bool hw_available() noexcept;

[[nodiscard]] std::uint32_t extend(std::uint32_t crc, std::span<const std::byte> data) noexcept;

[[nodiscard]] inline std::uint32_t value(std::span<const std::byte> data) noexcept {
  return extend(0, data);
}

}  // namespace mr::crc32c
