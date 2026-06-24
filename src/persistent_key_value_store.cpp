#include "nebulakv/persistent_key_value_store.hpp"

#include "nebulakv/validation.hpp"
#include "nebulakv/wal_record.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nebulakv {

namespace {

[[nodiscard]] std::filesystem::path
resolve_sstable_directory(const PersistentStoreOptions& options) {
  if (!options.sstable_directory.empty()) {
    return options.sstable_directory;
  }
  const std::filesystem::path parent = options.wal_path.parent_path();
  return (parent.empty() ? std::filesystem::path{"."} : parent) / "sstables";
}

[[nodiscard]] std::optional<Entry> latest_entry(const MemTableSet& memtables,
                                                const SSTableManager& sstables,
                                                const std::string_view key) {
  if (const auto memory_entry = memtables.latest_entry(key)) {
    return memory_entry;
  }
  return sstables.get(key);
}

class RecoveryDestination final : public KeyValueStore {
public:
  RecoveryDestination(MemTableSet& memtables, const SSTableManager& sstables,
                      std::atomic<std::size_t>& live_key_count)
      : memtables_{memtables}, sstables_{sstables}, live_key_count_{live_key_count} {}

  void put(std::string key, std::string value) override {
    const bool was_live = exists(key);
    memtables_.put(std::move(key), std::move(value));
    if (!was_live) {
      live_key_count_.fetch_add(1U, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] std::optional<std::string> get(const std::string_view key) const override {
    const auto entry = latest_entry(memtables_, sstables_, key);
    if (!entry || entry->deleted) {
      return std::nullopt;
    }
    return entry->value;
  }

  bool remove(const std::string_view key) override {
    if (!exists(key)) {
      return false;
    }
    memtables_.add_tombstone(std::string{key});
    live_key_count_.fetch_sub(1U, std::memory_order_relaxed);
    return true;
  }

  [[nodiscard]] bool exists(const std::string_view key) const override {
    const auto entry = latest_entry(memtables_, sstables_, key);
    return entry && !entry->deleted;
  }

private:
  MemTableSet& memtables_;
  const SSTableManager& sstables_;
  std::atomic<std::size_t>& live_key_count_;
};

} // namespace

PersistentKeyValueStore::PersistentKeyValueStore(PersistentStoreOptions options)
    : sstables_{SSTableManagerOptions{
          resolve_sstable_directory(options), options.sstable_data_block_bytes,
          options.block_cache_capacity_bytes, options.bloom_false_positive_rate,
          options.level0_compaction_trigger, options.level0_compaction_max_tables}},
      automatic_compaction_enabled_{options.enable_automatic_compaction} {
  if (options.wal_path.empty()) {
    throw std::invalid_argument{"WAL path must not be empty"};
  }

  memtables_ = std::make_unique<MemTableSet>(MemTableOptions{
      options.memtable_max_bytes, sstables_.max_sequence_number(), sstables_.next_generation()});
  live_key_count_.store(sstables_.live_key_count(), std::memory_order_relaxed);

  RecoveryOptions recovery_options;
  recovery_options.truncate_invalid_tail = options.truncate_invalid_wal_tail;
  recovery_options.diagnostics = options.emit_recovery_diagnostics ? &std::cerr : nullptr;

  RecoveryDestination recovery_destination{*memtables_, sstables_, live_key_count_};
  recovery_report_ =
      RecoveryManager::recover(options.wal_path, recovery_destination, recovery_options);
  if (recovery_report_.issue && recovery_report_.issue->code == WalReadIssueCode::IoError) {
    throw std::runtime_error{"failed to read the write-ahead log during recovery"};
  }
  if (recovery_report_.issue && !recovery_report_.invalid_tail_truncated) {
    throw std::runtime_error{
        "write-ahead log contains an invalid tail and automatic repair is disabled"};
  }

  WalWriterOptions writer_options;
  writer_options.path = std::move(options.wal_path);
  writer_options.durability_mode = options.durability_mode;
  writer_options.batch_flush_interval = options.batch_flush_interval;
  wal_writer_ = std::make_unique<WalWriter>(std::move(writer_options));
}

void PersistentKeyValueStore::put(std::string key, std::string value) {
  validate_key(key);
  validate_value(value);

  std::lock_guard lock{write_mutex_};
  const bool was_live = exists_without_validation(key);
  wal_writer_->append(WalRecord{OperationType::Put, key, value});
  memtables_->put(std::move(key), std::move(value));
  if (!was_live) {
    live_key_count_.fetch_add(1U, std::memory_order_relaxed);
  }
  flush_immutable_memtables_locked();
  compact_if_required_locked();
  reset_wal_if_fully_persisted_locked();
}

std::optional<std::string> PersistentKeyValueStore::get(const std::string_view key) const {
  validate_key(key);
  auto entry = latest_entry_without_validation(key);
  if (!entry || entry->deleted) {
    return std::nullopt;
  }
  return std::move(entry->value);
}

bool PersistentKeyValueStore::remove(const std::string_view key) {
  validate_key(key);

  std::lock_guard lock{write_mutex_};
  if (!exists_without_validation(key)) {
    return false;
  }

  wal_writer_->append(WalRecord{OperationType::Delete, std::string{key}, {}});
  memtables_->add_tombstone(std::string{key});
  live_key_count_.fetch_sub(1U, std::memory_order_relaxed);
  flush_immutable_memtables_locked();
  compact_if_required_locked();
  reset_wal_if_fully_persisted_locked();
  return true;
}

bool PersistentKeyValueStore::exists(const std::string_view key) const {
  validate_key(key);
  return exists_without_validation(key);
}

void PersistentKeyValueStore::flush() {
  std::lock_guard lock{write_mutex_};
  wal_writer_->flush();
}

void PersistentKeyValueStore::checkpoint() {
  std::lock_guard lock{write_mutex_};
  wal_writer_->flush();
  static_cast<void>(memtables_->rotate_active());
  flush_immutable_memtables_locked();
  compact_if_required_locked();
  wal_writer_->reset();
}

CompactionResult PersistentKeyValueStore::compact() {
  std::lock_guard lock{write_mutex_};
  wal_writer_->flush();
  static_cast<void>(memtables_->rotate_active());
  flush_immutable_memtables_locked();
  CompactionResult result = sstables_.compact_level0(true);
  reset_wal_if_fully_persisted_locked();
  return result;
}

std::size_t PersistentKeyValueStore::size() const noexcept {
  return live_key_count_.load(std::memory_order_acquire);
}

std::uint64_t PersistentKeyValueStore::last_sequence_number() const noexcept {
  return memtables_->last_sequence_number();
}

std::size_t PersistentKeyValueStore::immutable_memtable_count() const {
  return memtables_->immutable_table_count();
}

std::size_t PersistentKeyValueStore::active_memtable_memory_usage() const {
  return memtables_->active_memory_usage();
}

std::size_t PersistentKeyValueStore::sstable_count() const { return sstables_.table_count(); }

std::size_t PersistentKeyValueStore::level0_sstable_count() const {
  return sstables_.level_table_count(SSTableLevel::Level0);
}

std::size_t PersistentKeyValueStore::level1_sstable_count() const {
  return sstables_.level_table_count(SSTableLevel::Level1);
}

std::vector<SSTableMetadata> PersistentKeyValueStore::sstable_metadata() const {
  return sstables_.metadata();
}

BlockCacheStatistics PersistentKeyValueStore::block_cache_statistics() const {
  return sstables_.block_cache_statistics();
}

BloomFilterAggregateStatistics PersistentKeyValueStore::bloom_filter_statistics() const {
  return sstables_.bloom_filter_statistics();
}

CompactionStatistics PersistentKeyValueStore::compaction_statistics() const {
  return sstables_.compaction_statistics();
}

void PersistentKeyValueStore::clear_block_cache() { sstables_.clear_block_cache(); }

void PersistentKeyValueStore::reset_read_statistics() { sstables_.reset_read_statistics(); }

const std::filesystem::path& PersistentKeyValueStore::sstable_directory() const noexcept {
  return sstables_.directory();
}

const std::filesystem::path& PersistentKeyValueStore::current_path() const noexcept {
  return sstables_.current_path();
}

std::filesystem::path PersistentKeyValueStore::active_manifest_path() const {
  return sstables_.active_manifest_path();
}

const RecoveryReport& PersistentKeyValueStore::recovery_report() const noexcept {
  return recovery_report_;
}

DurabilityMode PersistentKeyValueStore::durability_mode() const noexcept {
  return wal_writer_->durability_mode();
}

std::optional<Entry>
PersistentKeyValueStore::latest_entry_without_validation(const std::string_view key) const {
  return latest_entry(*memtables_, sstables_, key);
}

bool PersistentKeyValueStore::exists_without_validation(const std::string_view key) const {
  const auto entry = latest_entry_without_validation(key);
  return entry && !entry->deleted;
}

void PersistentKeyValueStore::flush_immutable_memtables_locked() {
  std::vector<std::shared_ptr<const MemTable>> tables = memtables_->immutable_tables();
  std::reverse(tables.begin(), tables.end());
  for (const auto& table : tables) {
    static_cast<void>(sstables_.flush(*table));
    if (!memtables_->discard_immutable(table->generation())) {
      throw std::logic_error{"flushed MemTable was not registered as immutable"};
    }
  }
}

void PersistentKeyValueStore::reset_wal_if_fully_persisted_locked() {
  if (memtables_->active_entry_count() == 0U && memtables_->immutable_table_count() == 0U) {
    wal_writer_->reset();
  }
}

void PersistentKeyValueStore::compact_if_required_locked() {
  if (!automatic_compaction_enabled_) {
    return;
  }
  while (sstables_.needs_compaction()) {
    const CompactionResult result = sstables_.compact_if_needed();
    if (!result.performed) {
      break;
    }
  }
}

} // namespace nebulakv
