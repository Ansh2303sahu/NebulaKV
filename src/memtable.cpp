#include "nebulakv/memtable.hpp"

#include "nebulakv/validation.hpp"

#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace nebulakv {

MemTable::MemTable(const std::uint64_t generation) : generation_{generation} {}

void MemTable::put(std::string key, std::string value, const std::uint64_t sequence_number) {
  validate_key(key);
  validate_value(value);
  if (sequence_number == 0U) {
    throw std::invalid_argument{"MemTable sequence numbers must be greater than zero"};
  }

  insert_or_assign(std::move(key), Entry{std::move(value), sequence_number, false});
}

void MemTable::add_tombstone(std::string key, const std::uint64_t sequence_number) {
  validate_key(key);
  if (sequence_number == 0U) {
    throw std::invalid_argument{"MemTable sequence numbers must be greater than zero"};
  }

  insert_or_assign(std::move(key), Entry{{}, sequence_number, true});
}

std::optional<Entry> MemTable::get_entry(const std::string_view key) const {
  validate_key(key);

  std::shared_lock lock{mutex_};
  const auto entry = entries_.find(key);
  if (entry == entries_.end()) {
    return std::nullopt;
  }

  return entry->second;
}

std::optional<MemTable::LookupState> MemTable::lookup_state(const std::string_view key) const {
  validate_key(key);

  std::shared_lock lock{mutex_};
  const auto entry = entries_.find(key);
  if (entry == entries_.end()) {
    return std::nullopt;
  }

  return entry->second.deleted ? LookupState::Tombstone : LookupState::Value;
}

MemTable::Snapshot MemTable::snapshot() const {
  std::shared_lock lock{mutex_};
  Snapshot result;
  result.reserve(entries_.size());
  for (const auto& [key, entry] : entries_) {
    result.emplace_back(key, entry);
  }
  return result;
}

void MemTable::freeze() {
  std::unique_lock lock{mutex_};
  immutable_ = true;
}

bool MemTable::is_immutable() const {
  std::shared_lock lock{mutex_};
  return immutable_;
}

bool MemTable::empty() const {
  std::shared_lock lock{mutex_};
  return entries_.empty();
}

std::size_t MemTable::entry_count() const {
  std::shared_lock lock{mutex_};
  return entries_.size();
}

std::size_t MemTable::approximate_memory_usage() const {
  std::shared_lock lock{mutex_};
  return approximate_memory_usage_;
}

std::uint64_t MemTable::generation() const noexcept { return generation_; }

std::size_t MemTable::estimated_entry_size(const std::string_view key,
                                           const Entry& entry) noexcept {
  constexpr std::size_t kTreeNodeOverhead = sizeof(void*) * 4U;
  return sizeof(Storage::value_type) + kTreeNodeOverhead + key.size() + entry.value.size();
}

void MemTable::ensure_mutable() const {
  if (immutable_) {
    throw std::logic_error{"cannot modify an immutable MemTable"};
  }
}

void MemTable::insert_or_assign(std::string key, Entry entry) {
  std::unique_lock lock{mutex_};
  ensure_mutable();

  const auto existing = entries_.find(key);
  if (existing != entries_.end()) {
    if (entry.sequence_number <= existing->second.sequence_number) {
      throw std::invalid_argument{"MemTable updates require a newer sequence number"};
    }

    approximate_memory_usage_ -= estimated_entry_size(existing->first, existing->second);
    existing->second = std::move(entry);
    approximate_memory_usage_ += estimated_entry_size(existing->first, existing->second);
    return;
  }

  const auto [inserted, was_inserted] = entries_.emplace(std::move(key), std::move(entry));
  if (!was_inserted) {
    throw std::logic_error{"failed to insert MemTable entry"};
  }
  approximate_memory_usage_ += estimated_entry_size(inserted->first, inserted->second);
}

} // namespace nebulakv
