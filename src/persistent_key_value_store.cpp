#include "nebulakv/persistent_key_value_store.hpp"

#include "nebulakv/validation.hpp"
#include "nebulakv/wal_record.hpp"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace nebulakv {

PersistentKeyValueStore::PersistentKeyValueStore(PersistentStoreOptions options)
    : memtables_{MemTableOptions{options.memtable_max_bytes}} {
  RecoveryOptions recovery_options;
  recovery_options.truncate_invalid_tail = options.truncate_invalid_wal_tail;
  recovery_options.diagnostics = options.emit_recovery_diagnostics ? &std::cerr : nullptr;

  recovery_report_ = RecoveryManager::recover(options.wal_path, memtables_, recovery_options);
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
  wal_writer_->append(WalRecord{OperationType::Put, key, value});
  memtables_.put(std::move(key), std::move(value));
}

std::optional<std::string> PersistentKeyValueStore::get(const std::string_view key) const {
  return memtables_.get(key);
}

bool PersistentKeyValueStore::remove(const std::string_view key) {
  validate_key(key);

  std::lock_guard lock{write_mutex_};
  if (!memtables_.exists(key)) {
    return false;
  }

  wal_writer_->append(WalRecord{OperationType::Delete, std::string{key}, {}});
  return memtables_.remove(key);
}

bool PersistentKeyValueStore::exists(const std::string_view key) const {
  return memtables_.exists(key);
}

void PersistentKeyValueStore::flush() {
  std::lock_guard lock{write_mutex_};
  wal_writer_->flush();
}

std::size_t PersistentKeyValueStore::size() const { return memtables_.size(); }

std::uint64_t PersistentKeyValueStore::last_sequence_number() const noexcept {
  return memtables_.last_sequence_number();
}

std::size_t PersistentKeyValueStore::immutable_memtable_count() const {
  return memtables_.immutable_table_count();
}

std::size_t PersistentKeyValueStore::active_memtable_memory_usage() const {
  return memtables_.active_memory_usage();
}

const RecoveryReport& PersistentKeyValueStore::recovery_report() const noexcept {
  return recovery_report_;
}

DurabilityMode PersistentKeyValueStore::durability_mode() const noexcept {
  return wal_writer_->durability_mode();
}

} // namespace nebulakv
