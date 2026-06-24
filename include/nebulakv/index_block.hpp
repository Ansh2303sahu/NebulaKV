#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nebulakv {

struct IndexEntry {
  std::string first_key;
  std::string last_key;
  std::uint64_t block_offset{0};
  std::uint64_t block_size{0};
};

struct IndexBlock {
  std::vector<IndexEntry> entries;
};

}  // namespace nebulakv
