#pragma once

#include "nebulakv/block_cache.hpp"
#include "nebulakv/bloom_filter.hpp"
#include "nebulakv/data_block.hpp"
#include "nebulakv/entry.hpp"
#include "nebulakv/index_block.hpp"
#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_footer.hpp"
#include "nebulakv/sstable_metadata.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace nebulakv {

struct SSTableLookupStatistics {
  std::uint64_t bloom_checks{0};
  std::uint64_t bloom_negatives{0};
};

class SSTableReader final {
public:
  explicit SSTableReader(std::filesystem::path path, std::shared_ptr<BlockCache> block_cache = {},
                         std::shared_ptr<const BloomFilter> bloom_filter = {});

  [[nodiscard]] std::optional<Entry> get(std::string_view key) const;
  [[nodiscard]] MemTable::Snapshot read_all() const;
  [[nodiscard]] const SSTableMetadata& metadata() const noexcept;
  [[nodiscard]] const IndexBlock& index() const noexcept;
  [[nodiscard]] const SSTableFooter& footer() const noexcept;
  [[nodiscard]] bool uses_bloom_filter() const noexcept;
  [[nodiscard]] std::optional<BloomFilterStatistics> bloom_filter_statistics() const noexcept;
  [[nodiscard]] SSTableLookupStatistics lookup_statistics() const noexcept;
  void reset_lookup_statistics() noexcept;

private:
  [[nodiscard]] BlockCache::BlockPointer read_data_block(const IndexEntry& index_entry) const;

  std::filesystem::path path_;
  std::string cache_table_id_;
  SSTableMetadata metadata_;
  IndexBlock index_;
  SSTableFooter footer_;
  std::shared_ptr<BlockCache> block_cache_;
  std::shared_ptr<const BloomFilter> bloom_filter_;
  mutable std::atomic<std::uint64_t> bloom_checks_{0};
  mutable std::atomic<std::uint64_t> bloom_negatives_{0};
};

} // namespace nebulakv
