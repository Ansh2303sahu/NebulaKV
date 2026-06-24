#include "wal_format.hpp"

#include "nebulakv/checksum_calculator.hpp"
#include "nebulakv/validation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>

namespace nebulakv::wal_format {

namespace {

void append_bytes(std::vector<std::byte>& destination, const std::string_view value) {
  const auto* begin = reinterpret_cast<const std::byte*>(value.data());
  destination.insert(destination.end(), begin, begin + value.size());
}

} // namespace

void append_uint16_le(std::vector<std::byte>& destination, const std::uint16_t value) {
  destination.push_back(static_cast<std::byte>(value & 0xFFU));
  destination.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_uint32_le(std::vector<std::byte>& destination, const std::uint32_t value) {
  destination.push_back(static_cast<std::byte>(value & 0xFFU));
  destination.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
  destination.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
  destination.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
}

std::uint16_t read_uint16_le(const std::span<const std::byte> bytes) {
  if (bytes.size() < 2U) {
    throw std::invalid_argument{"insufficient bytes for uint16"};
  }

  const auto low = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[0]));
  const auto high = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[1]));
  return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8U));
}

std::uint32_t read_uint32_le(const std::span<const std::byte> bytes) {
  if (bytes.size() < 4U) {
    throw std::invalid_argument{"insufficient bytes for uint32"};
  }

  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4U; ++index) {
    const auto byte = static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index]));
    value |= byte << static_cast<unsigned int>(index * 8U);
  }
  return value;
}

std::vector<std::byte> serialize(const WalRecord& record) {
  validate_key(record.key);
  if (record.operation == OperationType::Put) {
    validate_value(record.value);
  } else if (record.operation == OperationType::Delete) {
    if (!record.value.empty()) {
      throw std::invalid_argument{"delete WAL records must not contain a value"};
    }
  } else {
    throw std::invalid_argument{"unsupported WAL operation"};
  }

  if (record.key.size() > std::numeric_limits<std::uint32_t>::max() ||
      record.value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error{"WAL record length exceeds the binary format"};
  }

  const auto key_size = static_cast<std::uint32_t>(record.key.size());
  const auto value_size = static_cast<std::uint32_t>(record.value.size());

  std::vector<std::byte> encoded;
  encoded.reserve(kHeaderSize + record.key.size() + record.value.size() + kChecksumSize);
  encoded.insert(encoded.end(), kMagic.begin(), kMagic.end());
  append_uint16_le(encoded, kVersion);
  encoded.push_back(static_cast<std::byte>(record.operation));
  encoded.push_back(std::byte{0});
  append_uint32_le(encoded, key_size);
  append_uint32_le(encoded, value_size);
  append_bytes(encoded, record.key);
  append_bytes(encoded, record.value);

  const std::uint32_t checksum = ChecksumCalculator::crc32(encoded);
  append_uint32_le(encoded, checksum);
  return encoded;
}

} // namespace nebulakv::wal_format
