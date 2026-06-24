#pragma once

#include "nebulakv/durability_mode.hpp"
#include "nebulakv/key_value_store.hpp"
#include "nebulakv/memtable_set.hpp"
#include "nebulakv/recovery_manager.hpp"
#include "nebulakv/sstable_manager.hpp"
#include "nebulakv/wal_writer.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nebulakv {

struct PersistentStoreOptions {
  std::filesystem::path wal_path;
  std::filesystem::path sstable_directory;
  DurabilityMode durability_mode{DurabilityMode::Sync};
  std::chrono::milliseconds batch_flush_interval{100};
  std::size_t memtable_max_bytes{64U * 1024U * 1024U};
  std::size_t sstable_data_block_bytes{32U * 1024U};
  std::size_t block_cache_capacity_bytes{64U * 1024U * 1024U};
  double bloom_false_positive_rate{0.01};
  std::size_t level0_compaction_trigger{4U};
  std::size_t level0_compaction_max_tables{4U};
  bool enable_automatic_compaction{true};
  bool truncate_invalid_wal_tail{true};
  bool emit_recovery_diagnostics{true};
};

class PersistentKeyValueStore final : public KeyValueStore {
 public:
  explicit PersistentKeyValueStore(PersistentStoreOptions options);
  ~PersistentKeyValueStore() override = default;

  PersistentKeyValueStore(const PersistentKeyValueStore&) = delete;
  PersistentKeyValueStore& operator=(const PersistentKeyValueStore&) = delete;
  PersistentKeyValueStore(PersistentKeyValueStore&&) = delete;
  PersistentKeyValueStore& operator=(PersistentKeyValueStore&&) = delete;

  void put(std::string key, std::string value) override;
  [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;
  bool remove(std::string_view key) override;
  [[nodiscard]] bool exists(std::string_view key) const override;

  void flush();
  void checkpoint();
  [[nodiscard]] CompactionResult compact();

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::uint64_t last_sequence_number() const noexcept;
  [[nodiscard]] std::size_t immutable_memtable_count() const;
  [[nodiscard]] std::size_t active_memtable_memory_usage() const;
  [[nodiscard]] std::size_t sstable_count() const;
  [[nodiscard]] std::size_t level0_sstable_count() const;
  [[nodiscard]] std::size_t level1_sstable_count() const;
  [[nodiscard]] std::vector<SSTableMetadata> sstable_metadata() const;
  [[nodiscard]] BlockCacheStatistics block_cache_statistics() const;
  [[nodiscard]] BloomFilterAggregateStatistics bloom_filter_statistics() const;
  [[nodiscard]] CompactionStatistics compaction_statistics() const;
  void clear_block_cache();
  void reset_read_statistics();
  [[nodiscard]] const std::filesystem::path& sstable_directory() const noexcept;
  [[nodiscard]] const std::filesystem::path& current_path() const noexcept;
  [[nodiscard]] std::filesystem::path active_manifest_path() const;
  [[nodiscard]] const RecoveryReport& recovery_report() const noexcept;
  [[nodiscard]] DurabilityMode durability_mode() const noexcept;

 private:
  [[nodiscard]] std::optional<Entry> latest_entry_without_validation(
      std::string_view key) const;
  [[nodiscard]] bool exists_without_validation(std::string_view key) const;
  void flush_immutable_memtables_locked();
  void reset_wal_if_fully_persisted_locked();
  void compact_if_required_locked();

  mutable std::mutex write_mutex_;
  SSTableManager sstables_;
  std::unique_ptr<MemTableSet> memtables_;
  std::atomic<std::size_t> live_key_count_{0};
  RecoveryReport recovery_report_;
  std::unique_ptr<WalWriter> wal_writer_;
  bool automatic_compaction_enabled_{true};
};

}  // namespace nebulakv
