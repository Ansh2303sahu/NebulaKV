#pragma once

#include "nebulakv/wal_record.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nebulakv::wal_format {

inline constexpr std::array<std::byte, 4> kMagic{
    std::byte{'N'}, std::byte{'K'}, std::byte{'V'}, std::byte{'W'}};
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 16;
inline constexpr std::size_t kChecksumSize = 4;

[[nodiscard]] std::vector<std::byte> serialize(const WalRecord& record);
[[nodiscard]] std::uint16_t read_uint16_le(std::span<const std::byte> bytes);
[[nodiscard]] std::uint32_t read_uint32_le(std::span<const std::byte> bytes);
void append_uint16_le(std::vector<std::byte>& destination, std::uint16_t value);
void append_uint32_le(std::vector<std::byte>& destination, std::uint32_t value);

}  // namespace nebulakv::wal_format
