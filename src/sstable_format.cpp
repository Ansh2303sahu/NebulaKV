#include "sstable_format.hpp"

#include "nebulakv/checksum_calculator.hpp"
#include "nebulakv/sstable_error.hpp"
#include "nebulakv/storage_limits.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nebulakv::sstable_format {

namespace {

void append_string(std::vector<std::byte>& destination, const std::string_view value) {
  const auto* bytes = reinterpret_cast<const std::byte*>(value.data());
  destination.insert(destination.end(), bytes, bytes + value.size());
}

[[nodiscard]] bool has_magic(const std::span<const std::byte> bytes,
                             const std::span<const std::byte> magic) {
  return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
}

void validate_checksum(const std::span<const std::byte> bytes, const std::uint64_t file_offset,
                       const std::string_view component) {
  if (bytes.size() < kChecksumSize) {
    throw SSTableCorruptionError{file_offset,
                                 std::string{component} + " is shorter than its checksum"};
  }

  const auto payload = bytes.first(bytes.size() - kChecksumSize);
  const std::uint32_t expected = read_uint32(bytes.last(kChecksumSize));
  const std::uint32_t actual = ChecksumCalculator::crc32(payload);
  if (expected != actual) {
    throw SSTableCorruptionError{file_offset + bytes.size() - kChecksumSize,
                                 std::string{component} + " checksum mismatch"};
  }
}

[[nodiscard]] std::string read_string(const std::span<const std::byte> bytes, std::size_t& cursor,
                                      const std::size_t length, const std::uint64_t file_offset,
                                      const std::string_view component) {
  if (cursor > bytes.size() || length > bytes.size() - cursor) {
    throw SSTableCorruptionError{file_offset + cursor,
                                 std::string{component} + " contains a truncated string"};
  }

  const auto* characters = reinterpret_cast<const char*>(bytes.data() + cursor);
  std::string result{characters, length};
  cursor += length;
  return result;
}

void require_size(const std::span<const std::byte> bytes, const std::size_t expected,
                  const std::uint64_t file_offset, const std::string_view component) {
  if (bytes.size() != expected) {
    throw SSTableCorruptionError{file_offset, std::string{component} + " has an invalid size"};
  }
}

} // namespace

void append_uint16(std::vector<std::byte>& destination, const std::uint16_t value) {
  destination.push_back(static_cast<std::byte>(value & 0xFFU));
  destination.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_uint32(std::vector<std::byte>& destination, const std::uint32_t value) {
  for (unsigned int shift = 0; shift < 32U; shift += 8U) {
    destination.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

void append_uint64(std::vector<std::byte>& destination, const std::uint64_t value) {
  for (unsigned int shift = 0; shift < 64U; shift += 8U) {
    destination.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
  }
}

std::uint16_t read_uint16(const std::span<const std::byte> bytes) {
  if (bytes.size() < sizeof(std::uint16_t)) {
    throw std::invalid_argument{"insufficient bytes for uint16"};
  }
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[0])) |
      static_cast<std::uint16_t>(static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[1]))
                                 << 8U));
}

std::uint32_t read_uint32(const std::span<const std::byte> bytes) {
  if (bytes.size() < sizeof(std::uint32_t)) {
    throw std::invalid_argument{"insufficient bytes for uint32"};
  }

  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(std::uint32_t); ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[index]))
             << static_cast<unsigned int>(index * 8U);
  }
  return value;
}

std::uint64_t read_uint64(const std::span<const std::byte> bytes) {
  if (bytes.size() < sizeof(std::uint64_t)) {
    throw std::invalid_argument{"insufficient bytes for uint64"};
  }

  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(std::uint64_t); ++index) {
    value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[index]))
             << static_cast<unsigned int>(index * 8U);
  }
  return value;
}

std::vector<std::byte> serialize_header(const Header& header) {
  std::vector<std::byte> bytes;
  bytes.reserve(kHeaderSize);
  bytes.insert(bytes.end(), kHeaderMagic.begin(), kHeaderMagic.end());
  append_uint16(bytes, kVersion);
  append_uint16(bytes, static_cast<std::uint16_t>(kHeaderSize));
  append_uint64(bytes, header.generation);
  append_uint64(bytes, header.entry_count);
  append_uint32(bytes, header.block_count);
  append_uint32(bytes, header.target_data_block_bytes);
  append_uint64(bytes, header.min_sequence_number);
  append_uint64(bytes, header.max_sequence_number);
  append_uint32(bytes, ChecksumCalculator::crc32(bytes));
  return bytes;
}

Header parse_header(const std::span<const std::byte> bytes, const std::uint64_t file_offset) {
  require_size(bytes, kHeaderSize, file_offset, "SSTable header");
  validate_checksum(bytes, file_offset, "SSTable header");
  if (!has_magic(bytes, kHeaderMagic)) {
    throw SSTableCorruptionError{file_offset, "SSTable header magic mismatch"};
  }
  if (read_uint16(bytes.subspan(8U, 2U)) != kVersion) {
    throw SSTableCorruptionError{file_offset + 8U, "unsupported SSTable version"};
  }
  if (read_uint16(bytes.subspan(10U, 2U)) != kHeaderSize) {
    throw SSTableCorruptionError{file_offset + 10U, "invalid SSTable header size"};
  }

  Header header;
  header.generation = read_uint64(bytes.subspan(12U, 8U));
  header.entry_count = read_uint64(bytes.subspan(20U, 8U));
  header.block_count = read_uint32(bytes.subspan(28U, 4U));
  header.target_data_block_bytes = read_uint32(bytes.subspan(32U, 4U));
  header.min_sequence_number = read_uint64(bytes.subspan(36U, 8U));
  header.max_sequence_number = read_uint64(bytes.subspan(44U, 8U));
  if (header.entry_count == 0U || header.block_count == 0U || header.min_sequence_number == 0U ||
      header.min_sequence_number > header.max_sequence_number) {
    throw SSTableCorruptionError{file_offset, "SSTable header metadata is invalid"};
  }
  return header;
}

std::vector<std::byte> serialize_data_block(const DataBlock& block) {
  if (block.records.empty()) {
    throw std::invalid_argument{"cannot serialize an empty SSTable data block"};
  }
  if (block.records.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error{"SSTable data block contains too many records"};
  }

  std::vector<std::byte> bytes;
  bytes.insert(bytes.end(), kDataBlockMagic.begin(), kDataBlockMagic.end());
  append_uint32(bytes, static_cast<std::uint32_t>(block.records.size()));

  std::string_view previous_key;
  for (const auto& [key, entry] : block.records) {
    if (key.empty() || key.size() > storage_limits::kMaxKeySize ||
        entry.value.size() > storage_limits::kMaxValueSize || entry.sequence_number == 0U ||
        (entry.deleted && !entry.value.empty())) {
      throw std::invalid_argument{"invalid entry supplied to SSTable data block"};
    }
    if (!previous_key.empty() && key <= previous_key) {
      throw std::invalid_argument{"SSTable data block keys must be strictly sorted"};
    }
    previous_key = key;

    append_uint32(bytes, static_cast<std::uint32_t>(key.size()));
    append_uint32(bytes, static_cast<std::uint32_t>(entry.value.size()));
    append_uint64(bytes, entry.sequence_number);
    bytes.push_back(entry.deleted ? std::byte{1} : std::byte{0});
    bytes.insert(bytes.end(), 3U, std::byte{0});
    append_string(bytes, key);
    append_string(bytes, entry.value);
  }

  append_uint32(bytes, ChecksumCalculator::crc32(bytes));
  return bytes;
}

DataBlock parse_data_block(const std::span<const std::byte> bytes,
                           const std::uint64_t file_offset) {
  constexpr std::size_t kPrefixSize = 12U;
  if (bytes.size() < kPrefixSize + kChecksumSize) {
    throw SSTableCorruptionError{file_offset, "SSTable data block is truncated"};
  }
  validate_checksum(bytes, file_offset, "SSTable data block");
  if (!has_magic(bytes, kDataBlockMagic)) {
    throw SSTableCorruptionError{file_offset, "SSTable data block magic mismatch"};
  }

  const std::uint32_t record_count = read_uint32(bytes.subspan(8U, 4U));
  if (record_count == 0U) {
    throw SSTableCorruptionError{file_offset + 8U, "SSTable data block is empty"};
  }

  DataBlock block;
  block.records.reserve(record_count);
  std::size_t cursor = kPrefixSize;
  const std::size_t payload_end = bytes.size() - kChecksumSize;
  std::string previous_key;

  for (std::uint32_t index = 0; index < record_count; ++index) {
    if (cursor > payload_end || kRecordFixedSize > payload_end - cursor) {
      throw SSTableCorruptionError{file_offset + cursor,
                                   "SSTable data block record header is truncated"};
    }

    const std::uint32_t key_size = read_uint32(bytes.subspan(cursor, 4U));
    const std::uint32_t value_size = read_uint32(bytes.subspan(cursor + 4U, 4U));
    const std::uint64_t sequence_number = read_uint64(bytes.subspan(cursor + 8U, 8U));
    const std::uint8_t flags = std::to_integer<std::uint8_t>(bytes[cursor + 16U]);
    cursor += kRecordFixedSize;

    if (key_size == 0U || key_size > storage_limits::kMaxKeySize ||
        value_size > storage_limits::kMaxValueSize || sequence_number == 0U || flags > 1U) {
      throw SSTableCorruptionError{file_offset + cursor - kRecordFixedSize,
                                   "SSTable data block record metadata is invalid"};
    }
    const bool deleted = flags == 1U;
    if (deleted && value_size != 0U) {
      throw SSTableCorruptionError{file_offset + cursor - 4U, "SSTable tombstone contains a value"};
    }

    std::string key =
        read_string(bytes.first(payload_end), cursor, key_size, file_offset, "SSTable data block");
    std::string value = read_string(bytes.first(payload_end), cursor, value_size, file_offset,
                                    "SSTable data block");
    if (!previous_key.empty() && key <= previous_key) {
      throw SSTableCorruptionError{file_offset + cursor,
                                   "SSTable data block keys are not strictly sorted"};
    }
    previous_key = key;
    block.records.emplace_back(std::move(key), Entry{std::move(value), sequence_number, deleted});
  }

  if (cursor != payload_end) {
    throw SSTableCorruptionError{file_offset + cursor,
                                 "SSTable data block contains trailing bytes"};
  }
  return block;
}

std::vector<std::byte> serialize_index_block(const IndexBlock& index) {
  if (index.entries.empty()) {
    throw std::invalid_argument{"cannot serialize an empty SSTable index"};
  }
  if (index.entries.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error{"SSTable index contains too many blocks"};
  }

  std::vector<std::byte> bytes;
  bytes.insert(bytes.end(), kIndexMagic.begin(), kIndexMagic.end());
  append_uint32(bytes, static_cast<std::uint32_t>(index.entries.size()));
  std::string_view previous_last_key;
  for (const IndexEntry& entry : index.entries) {
    if (entry.first_key.empty() || entry.last_key.empty() || entry.first_key > entry.last_key ||
        entry.block_size == 0U ||
        (!previous_last_key.empty() && entry.first_key <= previous_last_key)) {
      throw std::invalid_argument{"invalid SSTable index entry"};
    }
    previous_last_key = entry.last_key;
    append_uint64(bytes, entry.block_offset);
    append_uint64(bytes, entry.block_size);
    append_uint32(bytes, static_cast<std::uint32_t>(entry.first_key.size()));
    append_uint32(bytes, static_cast<std::uint32_t>(entry.last_key.size()));
    append_string(bytes, entry.first_key);
    append_string(bytes, entry.last_key);
  }
  append_uint32(bytes, ChecksumCalculator::crc32(bytes));
  return bytes;
}

IndexBlock parse_index_block(const std::span<const std::byte> bytes,
                             const std::uint64_t file_offset) {
  constexpr std::size_t kPrefixSize = 12U;
  constexpr std::size_t kEntryFixedSize = 24U;
  if (bytes.size() < kPrefixSize + kChecksumSize) {
    throw SSTableCorruptionError{file_offset, "SSTable index block is truncated"};
  }
  validate_checksum(bytes, file_offset, "SSTable index block");
  if (!has_magic(bytes, kIndexMagic)) {
    throw SSTableCorruptionError{file_offset, "SSTable index magic mismatch"};
  }

  const std::uint32_t entry_count = read_uint32(bytes.subspan(8U, 4U));
  if (entry_count == 0U) {
    throw SSTableCorruptionError{file_offset + 8U, "SSTable index is empty"};
  }

  IndexBlock index;
  index.entries.reserve(entry_count);
  std::size_t cursor = kPrefixSize;
  const std::size_t payload_end = bytes.size() - kChecksumSize;
  std::string previous_last_key;

  for (std::uint32_t position = 0; position < entry_count; ++position) {
    if (cursor > payload_end || kEntryFixedSize > payload_end - cursor) {
      throw SSTableCorruptionError{file_offset + cursor, "SSTable index entry is truncated"};
    }
    IndexEntry entry;
    entry.block_offset = read_uint64(bytes.subspan(cursor, 8U));
    entry.block_size = read_uint64(bytes.subspan(cursor + 8U, 8U));
    const std::uint32_t first_size = read_uint32(bytes.subspan(cursor + 16U, 4U));
    const std::uint32_t last_size = read_uint32(bytes.subspan(cursor + 20U, 4U));
    cursor += kEntryFixedSize;

    if (first_size == 0U || first_size > storage_limits::kMaxKeySize || last_size == 0U ||
        last_size > storage_limits::kMaxKeySize || entry.block_size == 0U) {
      throw SSTableCorruptionError{file_offset + cursor - kEntryFixedSize,
                                   "SSTable index metadata is invalid"};
    }
    entry.first_key =
        read_string(bytes.first(payload_end), cursor, first_size, file_offset, "SSTable index");
    entry.last_key =
        read_string(bytes.first(payload_end), cursor, last_size, file_offset, "SSTable index");
    if (entry.first_key > entry.last_key ||
        (!previous_last_key.empty() && entry.first_key <= previous_last_key)) {
      throw SSTableCorruptionError{file_offset + cursor, "SSTable index key ranges are invalid"};
    }
    previous_last_key = entry.last_key;
    index.entries.push_back(std::move(entry));
  }

  if (cursor != payload_end) {
    throw SSTableCorruptionError{file_offset + cursor, "SSTable index contains trailing bytes"};
  }
  return index;
}

std::vector<std::byte> serialize_footer(const SSTableFooter& footer) {
  std::vector<std::byte> bytes;
  bytes.reserve(kFooterSize);
  bytes.insert(bytes.end(), kFooterMagic.begin(), kFooterMagic.end());
  append_uint16(bytes, kVersion);
  append_uint16(bytes, static_cast<std::uint16_t>(kFooterSize));
  append_uint64(bytes, footer.index_offset);
  append_uint64(bytes, footer.index_size);
  append_uint64(bytes, footer.entry_count);
  append_uint32(bytes, footer.block_count);
  append_uint32(bytes, 0U);
  append_uint64(bytes, footer.min_sequence_number);
  append_uint64(bytes, footer.max_sequence_number);
  append_uint32(bytes, ChecksumCalculator::crc32(bytes));
  return bytes;
}

SSTableFooter parse_footer(const std::span<const std::byte> bytes,
                           const std::uint64_t file_offset) {
  require_size(bytes, kFooterSize, file_offset, "SSTable footer");
  validate_checksum(bytes, file_offset, "SSTable footer");
  if (!has_magic(bytes, kFooterMagic)) {
    throw SSTableCorruptionError{file_offset, "SSTable footer magic mismatch"};
  }
  if (read_uint16(bytes.subspan(8U, 2U)) != kVersion) {
    throw SSTableCorruptionError{file_offset + 8U, "unsupported SSTable footer version"};
  }
  if (read_uint16(bytes.subspan(10U, 2U)) != kFooterSize) {
    throw SSTableCorruptionError{file_offset + 10U, "invalid SSTable footer size"};
  }

  SSTableFooter footer;
  footer.index_offset = read_uint64(bytes.subspan(12U, 8U));
  footer.index_size = read_uint64(bytes.subspan(20U, 8U));
  footer.entry_count = read_uint64(bytes.subspan(28U, 8U));
  footer.block_count = read_uint32(bytes.subspan(36U, 4U));
  footer.min_sequence_number = read_uint64(bytes.subspan(44U, 8U));
  footer.max_sequence_number = read_uint64(bytes.subspan(52U, 8U));
  if (footer.index_size == 0U || footer.entry_count == 0U || footer.block_count == 0U ||
      footer.min_sequence_number == 0U || footer.min_sequence_number > footer.max_sequence_number) {
    throw SSTableCorruptionError{file_offset, "SSTable footer metadata is invalid"};
  }
  return footer;
}

std::size_t encoded_record_size(const DataBlock::Record& record) noexcept {
  return kRecordFixedSize + record.first.size() + record.second.value.size();
}

} // namespace nebulakv::sstable_format
