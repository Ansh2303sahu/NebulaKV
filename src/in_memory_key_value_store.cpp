#include "nebulakv/in_memory_key_value_store.hpp"
#include "nebulakv/validation.hpp"

#include <mutex>
#include <utility>

namespace nebulakv {

std::size_t InMemoryKeyValueStore::TransparentStringHash::operator()(
    const std::string_view value) const noexcept {
  return std::hash<std::string_view>{}(value);
}

std::size_t InMemoryKeyValueStore::TransparentStringHash::operator()(
    const std::string& value) const noexcept {
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


}  // namespace nebulakv
