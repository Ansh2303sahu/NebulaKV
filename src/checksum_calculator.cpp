#include "nebulakv/checksum_calculator.hpp"

#include <cstddef>
#include <cstdint>

namespace nebulakv {

std::uint32_t ChecksumCalculator::crc32(const std::span<const std::byte> data) noexcept {
  constexpr std::uint32_t kPolynomial = 0xEDB88320U;
  std::uint32_t checksum = 0xFFFFFFFFU;

  for (const std::byte value : data) {
    checksum ^= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(value));
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (checksum & 1U);
      checksum = (checksum >> 1U) ^ (kPolynomial & mask);
    }
  }

  return checksum ^ 0xFFFFFFFFU;
}

std::uint32_t ChecksumCalculator::crc32(const std::string_view data) noexcept {
  const auto* bytes = reinterpret_cast<const std::byte*>(data.data());
  return crc32(std::span<const std::byte>{bytes, data.size()});
}

}  // namespace nebulakv
