#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace nebulakv {

class ChecksumCalculator final {
 public:
  [[nodiscard]] static std::uint32_t crc32(std::span<const std::byte> data) noexcept;
  [[nodiscard]] static std::uint32_t crc32(std::string_view data) noexcept;
};

}  // namespace nebulakv
