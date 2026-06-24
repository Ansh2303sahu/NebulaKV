#pragma once

#include "nebulakv/sstable_level.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace nebulakv {

struct SSTableMetadata {
  std::filesystem::path path;
  std::uint64_t generation{0};
  std::uint64_t entry_count{0};
  std::uint32_t block_count{0};
  std::uint64_t min_sequence_number{0};
  std::uint64_t max_sequence_number{0};
  std::string smallest_key;
  std::string largest_key;
  SSTableLevel level{SSTableLevel::Level0};
};

} // namespace nebulakv
