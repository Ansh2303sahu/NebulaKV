#include "nebulakv/in_memory_key_value_store.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace nebulakv {

std::size_t InMemoryKeyValueStore::TransparentStringHash::operator()(
    const std::string_view value) const noexcept {
  return std::hash<std::string_view>{}(value);
}

std::size_t
InMemoryKeyValueStore::TransparentStringHash::operator()(const std::string& value) const noexcept {
  return (*this)(std::string_view{value});
}

void InMemoryKeyValueStore::put(std::string key, std::string value) {
  validate_key(key);
  validate_value(value);

  std::unique_lock lock{mutex_};
  entries_.insert_or_assign(std::move(key), std::move(value));
}

std::optional<std::string> InMemoryKeyValueStore::get(const std::string_view key) const {
  validate_key(key);

  std::shared_lock lock{mutex_};
  const auto entry = entries_.find(key);
  if (entry == entries_.end()) {
    return std::nullopt;
  }

  return entry->second;
}

bool InMemoryKeyValueStore::remove(const std::string_view key) {
  validate_key(key);

  std::unique_lock lock{mutex_};
  const auto entry = entries_.find(key);
  if (entry == entries_.end()) {
    return false;
  }

  entries_.erase(entry);
  return true;
}

bool InMemoryKeyValueStore::exists(const std::string_view key) const {
  validate_key(key);

  std::shared_lock lock{mutex_};
  return entries_.contains(key);
}

std::size_t InMemoryKeyValueStore::size() const {
  std::shared_lock lock{mutex_};
  return entries_.size();
}

void InMemoryKeyValueStore::validate_key(const std::string_view key) {
  if (key.empty()) {
    throw std::invalid_argument{"NebulaKV keys must not be empty"};
  }

  if (key.size() > kMaxKeySize) {
    throw std::length_error{"NebulaKV key exceeds the maximum size"};
  }
}

void InMemoryKeyValueStore::validate_value(const std::string_view value) {
  if (value.size() > kMaxValueSize) {
    throw std::length_error{"NebulaKV value exceeds the maximum size"};
  }
}

} // namespace nebulakv
