#pragma once

#include "nebulakv/data_block.hpp"
#include "nebulakv/entry.hpp"
#include "nebulakv/index_block.hpp"
#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_footer.hpp"
#include "nebulakv/sstable_metadata.hpp"

#include <filesystem>
#include <optional>
#include <string_view>

namespace nebulakv {

class SSTableReader final {
public:
  explicit SSTableReader(std::filesystem::path path);

  [[nodiscard]] std::optional<Entry> get(std::string_view key) const;
  [[nodiscard]] MemTable::Snapshot read_all() const;
  [[nodiscard]] const SSTableMetadata& metadata() const noexcept;
  [[nodiscard]] const IndexBlock& index() const noexcept;
  [[nodiscard]] const SSTableFooter& footer() const noexcept;

private:
  [[nodiscard]] DataBlock read_data_block(const IndexEntry& index_entry) const;

  std::filesystem::path path_;
  SSTableMetadata metadata_;
  IndexBlock index_;
  SSTableFooter footer_;
};

} // namespace nebulakv
