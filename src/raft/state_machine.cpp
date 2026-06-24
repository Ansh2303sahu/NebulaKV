#include "nebulakv/raft/state_machine.hpp"

#include "nebulakv/validation.hpp"
#include "serialization.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nebulakv::raft {

namespace {

constexpr std::uint32_t kStateSnapshotVersion = 1U;

} // namespace

DurableKeyValueStateMachine::DurableKeyValueStateMachine(DurableStateMachineOptions options)
    : options_{std::move(options)} {
  if (options_.directory.empty()) {
    throw std::invalid_argument{"state machine directory must not be empty"};
  }
  std::filesystem::create_directories(options_.directory);
}

DurableKeyValueStateMachine::~DurableKeyValueStateMachine() = default;

void DurableKeyValueStateMachine::recover(const std::optional<Snapshot>& snapshot,
                                          const std::vector<LogEntry>& committed_entries) {
  std::lock_guard lock{mutex_};
  reset_storage_locked();
  values_.clear();

  if (snapshot) {
    restore_map_locked(snapshot->payload);
    for (const auto& [key, value] : values_) {
      store_->put(key, value);
    }
  }

  for (const auto& entry : committed_entries) {
    apply_command_locked(entry.command);
  }
  store_->checkpoint();
}

void DurableKeyValueStateMachine::apply(const LogEntry& entry) {
  if (entry.index == 0U || entry.term == 0U) {
    throw std::invalid_argument{"committed Raft entry must have an index and term"};
  }
  std::lock_guard lock{mutex_};
  if (!store_) {
    open_storage_locked();
  }
  apply_command_locked(entry.command);
}

std::optional<std::string> DurableKeyValueStateMachine::get(const std::string_view key) const {
  validate_key(key);
  std::lock_guard lock{mutex_};
  const auto iterator = values_.find(key);
  if (iterator == values_.end()) {
    return std::nullopt;
  }
  return iterator->second;
}

std::string DurableKeyValueStateMachine::create_snapshot() const {
  std::lock_guard lock{mutex_};
  return encode_map_locked();
}

void DurableKeyValueStateMachine::install_snapshot(const std::string_view payload) {
  std::lock_guard lock{mutex_};
  reset_storage_locked();
  values_.clear();
  restore_map_locked(payload);
  for (const auto& [key, value] : values_) {
    store_->put(key, value);
  }
  store_->checkpoint();
}

void DurableKeyValueStateMachine::flush() {
  std::lock_guard lock{mutex_};
  if (store_) {
    store_->flush();
  }
}

std::size_t DurableKeyValueStateMachine::size() const noexcept {
  std::lock_guard lock{mutex_};
  return values_.size();
}

StateMachineStatistics DurableKeyValueStateMachine::statistics() const {
  std::lock_guard lock{mutex_};
  if (!store_) {
    return {};
  }
  const auto cache = store_->block_cache_statistics();
  const auto compaction = store_->compaction_statistics();
  StateMachineStatistics result;
  result.last_sequence_number = store_->last_sequence_number();
  result.level0_sstables = store_->level0_sstable_count();
  result.level1_sstables = store_->level1_sstable_count();
  result.cache_hits = cache.hits;
  result.cache_misses = cache.misses;
  result.cache_evictions = cache.evictions;
  result.cache_hit_ratio = cache.hit_ratio();
  result.compactions_completed = compaction.runs;
  return result;
}

PersistentKeyValueStore& DurableKeyValueStateMachine::storage() {
  std::lock_guard lock{mutex_};
  if (!store_) {
    open_storage_locked();
  }
  return *store_;
}

const PersistentKeyValueStore& DurableKeyValueStateMachine::storage() const {
  std::lock_guard lock{mutex_};
  if (!store_) {
    throw std::logic_error{"state machine has not been recovered"};
  }
  return *store_;
}

const std::filesystem::path& DurableKeyValueStateMachine::directory() const noexcept {
  return options_.directory;
}

void DurableKeyValueStateMachine::reset_storage_locked() {
  store_.reset();
  const auto state_directory = options_.directory / "state";
  std::error_code removal_error;
  std::filesystem::remove_all(state_directory, removal_error);
  if (removal_error) {
    throw std::filesystem::filesystem_error{"failed to reset state machine", state_directory,
                                            removal_error};
  }
  open_storage_locked();
}

void DurableKeyValueStateMachine::open_storage_locked() {
  const auto state_directory = options_.directory / "state";
  std::filesystem::create_directories(state_directory);

  PersistentStoreOptions store_options;
  store_options.wal_path = state_directory / "nebulakv.wal";
  store_options.sstable_directory = state_directory / "sstables";
  store_options.durability_mode = options_.durability_mode;
  store_options.memtable_max_bytes = options_.memtable_max_bytes;
  store_options.block_cache_capacity_bytes = options_.block_cache_capacity_bytes;
  store_ = std::make_unique<PersistentKeyValueStore>(std::move(store_options));
}

void DurableKeyValueStateMachine::apply_command_locked(const Command& command) {
  switch (command.type) {
  case CommandType::Noop:
    return;
  case CommandType::Put:
    validate_key(command.key);
    validate_value(command.value);
    store_->put(command.key, command.value);
    values_.insert_or_assign(command.key, command.value);
    return;
  case CommandType::Delete:
    validate_key(command.key);
    static_cast<void>(store_->remove(command.key));
    values_.erase(command.key);
    return;
  }
  throw std::invalid_argument{"unsupported Raft command"};
}

void DurableKeyValueStateMachine::restore_map_locked(const std::string_view payload) {
  detail::Decoder decoder{payload};
  if (decoder.read_u32() != kStateSnapshotVersion) {
    throw std::runtime_error{"unsupported state machine snapshot version"};
  }
  const auto count = static_cast<std::size_t>(decoder.read_u64());
  for (std::size_t index = 0U; index < count; ++index) {
    std::string key = decoder.read_bytes();
    std::string value = decoder.read_bytes();
    validate_key(key);
    validate_value(value);
    const auto [iterator, inserted] = values_.emplace(std::move(key), std::move(value));
    static_cast<void>(iterator);
    if (!inserted) {
      throw std::runtime_error{"duplicate key in state machine snapshot"};
    }
  }
  if (!decoder.empty()) {
    throw std::runtime_error{"unexpected bytes in state machine snapshot"};
  }
}

std::string DurableKeyValueStateMachine::encode_map_locked() const {
  std::string payload;
  detail::append_u32(payload, kStateSnapshotVersion);
  detail::append_u64(payload, static_cast<std::uint64_t>(values_.size()));
  for (const auto& [key, value] : values_) {
    detail::append_bytes(payload, key);
    detail::append_bytes(payload, value);
  }
  return payload;
}

} // namespace nebulakv::raft
