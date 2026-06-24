#pragma once

#include <cstdint>

namespace nebulakv {

struct SSTableFooter {
  std::uint64_t index_offset{0};
  std::uint64_t index_size{0};
  std::uint64_t entry_count{0};
  std::uint32_t block_count{0};
  std::uint64_t min_sequence_number{0};
  std::uint64_t max_sequence_number{0};
};

}  // namespace nebulakv
