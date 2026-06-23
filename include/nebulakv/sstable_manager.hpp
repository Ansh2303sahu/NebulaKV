#pragma once

#include "nebulakv/block_cache.hpp"
#include "nebulakv/bloom_filter.hpp"
#include "nebulakv/entry.hpp"
#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_metadata.hpp"
#include "nebulakv/sstable_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string_view>
#include <vector>

namespace nebulakv {

struct SSTableManagerOptions {
  std::filesystem::path directory;
  std::size_t target_data_block_bytes{32U * 1024U};
  std::size_t block_cache_capacity_bytes{64U * 1024U * 1024U};
  double bloom_false_positive_rate{0.01};
};

struct BloomFilterAggregateStatistics {
  std::size_t filter_count{0};
  std::size_t inserted_keys{0};
  std::size_t memory_bytes{0};
  std::uint64_t checks{0};
  std::uint64_t negative_results{0};
};

class SSTableManager final {
public:
  explicit SSTableManager(SSTableManagerOptions options);

  [[nodiscard]] SSTableMetadata flush(const MemTable& table);
  [[nodiscard]] std::optional<Entry> get(std::string_view key) const;
  [[nodiscard]] std::size_t table_count() const;
  [[nodiscard]] std::size_t live_key_count() const;
  [[nodiscard]] std::uint64_t max_sequence_number() const;
  [[nodiscard]] std::uint64_t next_generation() const;
  [[nodiscard]] std::vector<SSTableMetadata> metadata() const;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept;

  [[nodiscard]] BlockCacheStatistics block_cache_statistics() const;
  [[nodiscard]] BloomFilterAggregateStatistics bloom_filter_statistics() const;
  void clear_block_cache();
  void reset_read_statistics();

private:
  void load_existing();
  void add_reader(std::shared_ptr<SSTableReader> reader);
  [[nodiscard]] std::filesystem::path table_path(std::uint64_t generation,
                                                 std::uint64_t max_sequence) const;
  [[nodiscard]] std::shared_ptr<const BloomFilter>
  build_bloom_filter(const MemTable::Snapshot& snapshot) const;

  const std::filesystem::path directory_;
  const std::size_t target_data_block_bytes_{0};
  const double bloom_false_positive_rate_{0.0};
  std::shared_ptr<BlockCache> block_cache_;
  mutable std::shared_mutex mutex_;
  std::vector<std::shared_ptr<SSTableReader>> readers_;
  std::size_t live_key_count_{0};
  std::uint64_t max_sequence_number_{0};
  std::uint64_t next_generation_{0};
};

} // namespace nebulakv
