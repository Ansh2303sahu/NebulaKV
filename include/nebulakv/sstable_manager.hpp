#pragma once

#include "nebulakv/block_cache.hpp"
#include "nebulakv/bloom_filter.hpp"
#include "nebulakv/entry.hpp"
#include "nebulakv/manifest.hpp"
#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_level.hpp"
#include "nebulakv/sstable_metadata.hpp"
#include "nebulakv/sstable_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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
  std::size_t level0_compaction_trigger{4U};
  std::size_t level0_compaction_max_tables{4U};
};

struct BloomFilterAggregateStatistics {
  std::size_t filter_count{0};
  std::size_t inserted_keys{0};
  std::size_t memory_bytes{0};
  std::uint64_t checks{0};
  std::uint64_t negative_results{0};
};

struct CompactionResult {
  bool performed{false};
  std::size_t input_tables{0};
  std::size_t output_tables{0};
  std::uint64_t input_entries{0};
  std::uint64_t output_entries{0};
  std::uint64_t tombstones_dropped{0};
  std::uint64_t output_file_id{0};
};

struct CompactionStatistics {
  std::uint64_t runs{0};
  std::uint64_t input_tables{0};
  std::uint64_t output_tables{0};
  std::uint64_t input_entries{0};
  std::uint64_t output_entries{0};
  std::uint64_t tombstones_dropped{0};
};

class SSTableManager final {
 public:
  explicit SSTableManager(SSTableManagerOptions options);

  [[nodiscard]] SSTableMetadata flush(const MemTable& table);
  [[nodiscard]] CompactionResult compact_level0(bool force_all = false);
  [[nodiscard]] CompactionResult compact_if_needed();
  [[nodiscard]] bool needs_compaction() const;

  [[nodiscard]] std::optional<Entry> get(std::string_view key) const;
  [[nodiscard]] std::size_t table_count() const;
  [[nodiscard]] std::size_t level_table_count(SSTableLevel level) const;
  [[nodiscard]] std::size_t live_key_count() const;
  [[nodiscard]] std::uint64_t max_sequence_number() const;
  [[nodiscard]] std::uint64_t next_generation() const;
  [[nodiscard]] std::vector<SSTableMetadata> metadata() const;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept;
  [[nodiscard]] const std::filesystem::path& current_path() const noexcept;
  [[nodiscard]] std::filesystem::path active_manifest_path() const;
  [[nodiscard]] CompactionStatistics compaction_statistics() const;

  [[nodiscard]] BlockCacheStatistics block_cache_statistics() const;
  [[nodiscard]] BloomFilterAggregateStatistics bloom_filter_statistics() const;
  void clear_block_cache();
  void reset_read_statistics();

 private:
  struct ManagedTable {
    std::shared_ptr<SSTableReader> reader;
    SSTableLevel level{SSTableLevel::Level0};
  };

  struct CompactionSelection {
    std::vector<ManagedTable> selected;
    std::vector<ManagedTable> unselected;
    std::string smallest_key;
    std::string largest_key;
  };

  void load_existing();
  void load_manifest_snapshot(const ManifestSnapshot& snapshot);
  void migrate_legacy_tables();
  void cleanup_abandoned_files(const std::vector<SSTableMetadata>& active_tables) const;
  void publish_tables_locked(std::vector<ManagedTable> tables,
                             std::size_t live_key_count);
  void recalculate_state_locked();
  [[nodiscard]] CompactionSelection select_level0_compaction_locked(
      bool force_all) const;
  [[nodiscard]] bool can_drop_tombstone(
      std::string_view key, const Entry& tombstone,
      const std::vector<ManagedTable>& unselected) const;
  [[nodiscard]] std::vector<SSTableMetadata> metadata_for(
      const std::vector<ManagedTable>& tables) const;
  [[nodiscard]] std::uint64_t reserve_file_id();
  [[nodiscard]] std::filesystem::path table_path(
      SSTableLevel level, std::uint64_t file_id, std::uint64_t generation,
      std::uint64_t max_sequence) const;
  [[nodiscard]] std::shared_ptr<const BloomFilter> build_bloom_filter(
      const MemTable::Snapshot& snapshot) const;

  const std::filesystem::path directory_;
  const std::size_t target_data_block_bytes_{0};
  const double bloom_false_positive_rate_{0.0};
  const std::size_t level0_compaction_trigger_{0};
  const std::size_t level0_compaction_max_tables_{0};
  std::shared_ptr<BlockCache> block_cache_;
  ManifestManager manifest_;
  mutable std::shared_mutex mutex_;
  std::mutex compaction_mutex_;
  std::vector<ManagedTable> readers_;
  std::size_t live_key_count_{0};
  std::uint64_t max_sequence_number_{0};
  std::uint64_t next_generation_{0};
  std::uint64_t next_file_id_{1};
  CompactionStatistics compaction_statistics_;
  std::uint64_t state_version_{0};
};

}  // namespace nebulakv
