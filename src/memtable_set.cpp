#include "nebulakv/memtable_set.hpp"

#include "nebulakv/validation.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nebulakv {

MemTableSet::MemTableSet(const MemTableOptions options)
    : max_memory_bytes_{options.max_memory_bytes},
      active_{std::make_shared<MemTable>(options.initial_generation)},
      next_generation_{options.initial_generation + 1U},
      last_sequence_number_{options.initial_sequence_number} {
  if (max_memory_bytes_ == 0U) {
    throw std::invalid_argument{"MemTable memory limit must be greater than zero"};
  }
}

void MemTableSet::put(std::string key, std::string value) {
  validate_key(key);
  validate_value(value);

  std::lock_guard write_lock{write_mutex_};
  const auto previous_state = latest_state_without_validation(key);
  const bool was_live = previous_state == MemTable::LookupState::Value;
  const std::uint64_t sequence_number = next_sequence_number();
  const auto table = active_table();

  table->put(std::move(key), std::move(value), sequence_number);
  if (!was_live) {
    live_key_count_.fetch_add(1U, std::memory_order_relaxed);
  }
  last_sequence_number_.store(sequence_number, std::memory_order_release);
  rotate_if_required(table);
}

std::optional<std::string> MemTableSet::get(const std::string_view key) const {
  auto entry = latest_entry(key);
  if (!entry || entry->deleted) {
    return std::nullopt;
  }
  return std::move(entry->value);
}

bool MemTableSet::remove(const std::string_view key) {
  validate_key(key);

  std::lock_guard write_lock{write_mutex_};
  const auto previous_state = latest_state_without_validation(key);
  if (previous_state != MemTable::LookupState::Value) {
    return false;
  }

  const std::uint64_t sequence_number = next_sequence_number();
  const auto table = active_table();
  table->add_tombstone(std::string{key}, sequence_number);
  live_key_count_.fetch_sub(1U, std::memory_order_relaxed);
  last_sequence_number_.store(sequence_number, std::memory_order_release);
  rotate_if_required(table);
  return true;
}

void MemTableSet::add_tombstone(std::string key) {
  validate_key(key);

  std::lock_guard write_lock{write_mutex_};
  const bool was_live = latest_state_without_validation(key) == MemTable::LookupState::Value;
  const std::uint64_t sequence_number = next_sequence_number();
  const auto table = active_table();
  table->add_tombstone(std::move(key), sequence_number);
  if (was_live) {
    live_key_count_.fetch_sub(1U, std::memory_order_relaxed);
  }
  last_sequence_number_.store(sequence_number, std::memory_order_release);
  rotate_if_required(table);
}

bool MemTableSet::exists(const std::string_view key) const {
  validate_key(key);
  return latest_state_without_validation(key) == MemTable::LookupState::Value;
}

std::optional<Entry> MemTableSet::latest_entry(const std::string_view key) const {
  validate_key(key);
  return latest_entry_without_validation(key);
}

std::size_t MemTableSet::size() const noexcept {
  return live_key_count_.load(std::memory_order_acquire);
}

std::uint64_t MemTableSet::last_sequence_number() const noexcept {
  return last_sequence_number_.load(std::memory_order_acquire);
}

std::size_t MemTableSet::immutable_table_count() const {
  std::shared_lock lock{state_mutex_};
  return immutable_tables_.size();
}

std::size_t MemTableSet::active_memory_usage() const {
  return active_table()->approximate_memory_usage();
}

std::size_t MemTableSet::active_entry_count() const { return active_table()->entry_count(); }

std::uint64_t MemTableSet::active_generation() const { return active_table()->generation(); }

std::size_t MemTableSet::max_memory_bytes() const noexcept { return max_memory_bytes_; }

std::optional<std::shared_ptr<const MemTable>> MemTableSet::rotate_active() {
  std::lock_guard write_lock{write_mutex_};
  return rotate_active_locked();
}

std::vector<std::shared_ptr<const MemTable>> MemTableSet::immutable_tables() const {
  std::shared_lock lock{state_mutex_};
  return {immutable_tables_.begin(), immutable_tables_.end()};
}

bool MemTableSet::discard_immutable(const std::uint64_t generation) {
  std::unique_lock lock{state_mutex_};
  const auto table = std::find_if(
      immutable_tables_.begin(), immutable_tables_.end(),
      [generation](const auto& candidate) { return candidate->generation() == generation; });
  if (table == immutable_tables_.end()) {
    return false;
  }
  immutable_tables_.erase(table);
  return true;
}

std::optional<Entry>
MemTableSet::latest_entry_without_validation(const std::string_view key) const {
  std::shared_ptr<MemTable> active;
  std::vector<std::shared_ptr<const MemTable>> immutable;
  {
    std::shared_lock lock{state_mutex_};
    active = active_;
    immutable.assign(immutable_tables_.begin(), immutable_tables_.end());
  }

  if (const auto entry = active->get_entry(key)) {
    return entry;
  }
  for (const auto& table : immutable) {
    if (const auto entry = table->get_entry(key)) {
      return entry;
    }
  }
  return std::nullopt;
}

std::optional<MemTable::LookupState>
MemTableSet::latest_state_without_validation(const std::string_view key) const {
  std::shared_ptr<MemTable> active;
  std::vector<std::shared_ptr<const MemTable>> immutable;
  {
    std::shared_lock lock{state_mutex_};
    active = active_;
    immutable.assign(immutable_tables_.begin(), immutable_tables_.end());
  }

  if (const auto state = active->lookup_state(key)) {
    return state;
  }
  for (const auto& table : immutable) {
    if (const auto state = table->lookup_state(key)) {
      return state;
    }
  }
  return std::nullopt;
}

std::shared_ptr<MemTable> MemTableSet::active_table() const {
  std::shared_lock lock{state_mutex_};
  return active_;
}

void MemTableSet::rotate_if_required(const std::shared_ptr<MemTable>& table) {
  if (table->approximate_memory_usage() < max_memory_bytes_) {
    return;
  }

  std::unique_lock lock{state_mutex_};
  if (active_ != table || table->empty()) {
    return;
  }

  table->freeze();
  immutable_tables_.push_front(table);
  active_ = std::make_shared<MemTable>(next_generation_++);
}

std::optional<std::shared_ptr<const MemTable>> MemTableSet::rotate_active_locked() {
  std::unique_lock lock{state_mutex_};
  if (active_->empty()) {
    return std::nullopt;
  }

  active_->freeze();
  const std::shared_ptr<const MemTable> immutable = active_;
  immutable_tables_.push_front(immutable);
  active_ = std::make_shared<MemTable>(next_generation_++);
  return immutable;
}

std::uint64_t MemTableSet::next_sequence_number() const {
  const std::uint64_t current = last_sequence_number_.load(std::memory_order_relaxed);
  if (current == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error{"MemTable sequence number space is exhausted"};
  }
  return current + 1U;
}

} // namespace nebulakv
