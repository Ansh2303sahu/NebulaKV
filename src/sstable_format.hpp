#pragma once

#include "nebulakv/data_block.hpp"
#include "nebulakv/index_block.hpp"
#include "nebulakv/sstable_footer.hpp"
#include "nebulakv/sstable_metadata.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace nebulakv::sstable_format {

inline constexpr std::array<std::byte, 8> kHeaderMagic{
    std::byte{'N'}, std::byte{'B'}, std::byte{'L'}, std::byte{'S'},
    std::byte{'S'}, std::byte{'T'}, std::byte{'0'}, std::byte{'1'}};
inline constexpr std::array<std::byte, 8> kDataBlockMagic{
    std::byte{'N'}, std::byte{'B'}, std::byte{'L'}, std::byte{'D'},
    std::byte{'A'}, std::byte{'T'}, std::byte{'0'}, std::byte{'1'}};
inline constexpr std::array<std::byte, 8> kIndexMagic{
    std::byte{'N'}, std::byte{'B'}, std::byte{'L'}, std::byte{'I'},
    std::byte{'D'}, std::byte{'X'}, std::byte{'0'}, std::byte{'1'}};
inline constexpr std::array<std::byte, 8> kFooterMagic{
    std::byte{'N'}, std::byte{'B'}, std::byte{'L'}, std::byte{'F'},
    std::byte{'T'}, std::byte{'R'}, std::byte{'0'}, std::byte{'1'}};

inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 56;
inline constexpr std::size_t kFooterSize = 64;
inline constexpr std::size_t kChecksumSize = sizeof(std::uint32_t);
inline constexpr std::size_t kRecordFixedSize = 20;

struct Header {
  std::uint64_t generation{0};
  std::uint64_t entry_count{0};
  std::uint32_t block_count{0};
  std::uint32_t target_data_block_bytes{0};
  std::uint64_t min_sequence_number{0};
  std::uint64_t max_sequence_number{0};
};

void append_uint16(std::vector<std::byte>& destination, std::uint16_t value);
void append_uint32(std::vector<std::byte>& destination, std::uint32_t value);
void append_uint64(std::vector<std::byte>& destination, std::uint64_t value);
[[nodiscard]] std::uint16_t read_uint16(std::span<const std::byte> bytes);
[[nodiscard]] std::uint32_t read_uint32(std::span<const std::byte> bytes);
[[nodiscard]] std::uint64_t read_uint64(std::span<const std::byte> bytes);

[[nodiscard]] std::vector<std::byte> serialize_header(const Header& header);
[[nodiscard]] Header parse_header(std::span<const std::byte> bytes, std::uint64_t file_offset = 0);
[[nodiscard]] std::vector<std::byte> serialize_data_block(const DataBlock& block);
[[nodiscard]] DataBlock parse_data_block(std::span<const std::byte> bytes,
                                         std::uint64_t file_offset);
[[nodiscard]] std::vector<std::byte> serialize_index_block(const IndexBlock& index);
[[nodiscard]] IndexBlock parse_index_block(std::span<const std::byte> bytes,
                                           std::uint64_t file_offset);
[[nodiscard]] std::vector<std::byte> serialize_footer(const SSTableFooter& footer);
[[nodiscard]] SSTableFooter parse_footer(std::span<const std::byte> bytes,
                                         std::uint64_t file_offset);

[[nodiscard]] std::size_t encoded_record_size(const DataBlock::Record& record) noexcept;

} // namespace nebulakv::sstable_format
