#pragma once

#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace nebulakv {

struct SSTableWriterOptions {
  std::filesystem::path output_path;
  std::size_t target_data_block_bytes{32U * 1024U};
  std::uint64_t generation{0};
};

class SSTableWriter final {
public:
  [[nodiscard]] static SSTableMetadata write(const MemTable::Snapshot& entries,
                                             SSTableWriterOptions options);
};

} // namespace nebulakv
