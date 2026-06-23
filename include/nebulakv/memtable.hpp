#pragma once

#include "nebulakv/entry.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nebulakv {

class MemTable final {
public:
  using Snapshot = std::vector<std::pair<std::string, Entry>>;

  enum class LookupState : std::uint8_t {
    Value,
    Tombstone,
  };

  explicit MemTable(std::uint64_t generation);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;
  MemTable(MemTable&&) = delete;
  MemTable& operator=(MemTable&&) = delete;

  void put(std::string key, std::string value, std::uint64_t sequence_number);
  void add_tombstone(std::string key, std::uint64_t sequence_number);

  [[nodiscard]] std::optional<Entry> get_entry(std::string_view key) const;
  [[nodiscard]] std::optional<LookupState> lookup_state(std::string_view key) const;
  [[nodiscard]] Snapshot snapshot() const;

  void freeze();

  [[nodiscard]] bool is_immutable() const;
  [[nodiscard]] bool empty() const;
  [[nodiscard]] std::size_t entry_count() const;
  [[nodiscard]] std::size_t approximate_memory_usage() const;
  [[nodiscard]] std::uint64_t generation() const noexcept;

private:
  using Storage = std::map<std::string, Entry, std::less<>>;

  [[nodiscard]] static std::size_t estimated_entry_size(std::string_view key,
                                                        const Entry& entry) noexcept;
  void ensure_mutable() const;
  void insert_or_assign(std::string key, Entry entry);

  const std::uint64_t generation_{0};
  mutable std::shared_mutex mutex_;
  Storage entries_;
  std::size_t approximate_memory_usage_{0};
  bool immutable_{false};
};

} // namespace nebulakv
