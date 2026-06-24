#pragma once

#include "nebulakv/entry.hpp"
#include "nebulakv/key_value_store.hpp"
#include "nebulakv/memtable.hpp"
#include "nebulakv/storage_limits.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nebulakv {

struct MemTableOptions {
  std::size_t max_memory_bytes{64U * 1024U * 1024U};
  std::uint64_t initial_sequence_number{0};
  std::uint64_t initial_generation{0};
};

class MemTableSet final : public KeyValueStore {
public:
  static constexpr std::size_t kMaxKeySize = storage_limits::kMaxKeySize;
  static constexpr std::size_t kMaxValueSize = storage_limits::kMaxValueSize;

  explicit MemTableSet(MemTableOptions options = {});
  ~MemTableSet() override = default;

  MemTableSet(const MemTableSet&) = delete;
  MemTableSet& operator=(const MemTableSet&) = delete;
  MemTableSet(MemTableSet&&) = delete;
  MemTableSet& operator=(MemTableSet&&) = delete;

  void put(std::string key, std::string value) override;
  [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;
  bool remove(std::string_view key) override;
  void add_tombstone(std::string key);
  [[nodiscard]] bool exists(std::string_view key) const override;

  [[nodiscard]] std::optional<Entry> latest_entry(std::string_view key) const;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::uint64_t last_sequence_number() const noexcept;
  [[nodiscard]] std::size_t immutable_table_count() const;
  [[nodiscard]] std::size_t active_memory_usage() const;
  [[nodiscard]] std::size_t active_entry_count() const;
  [[nodiscard]] std::uint64_t active_generation() const;
  [[nodiscard]] std::size_t max_memory_bytes() const noexcept;

  [[nodiscard]] std::optional<std::shared_ptr<const MemTable>> rotate_active();
  [[nodiscard]] std::vector<std::shared_ptr<const MemTable>> immutable_tables() const;
  bool discard_immutable(std::uint64_t generation);

private:
  [[nodiscard]] std::optional<Entry> latest_entry_without_validation(std::string_view key) const;
  [[nodiscard]] std::optional<MemTable::LookupState>
  latest_state_without_validation(std::string_view key) const;
  [[nodiscard]] std::shared_ptr<MemTable> active_table() const;
  void rotate_if_required(const std::shared_ptr<MemTable>& table);
  [[nodiscard]] std::optional<std::shared_ptr<const MemTable>> rotate_active_locked();
  [[nodiscard]] std::uint64_t next_sequence_number() const;

  const std::size_t max_memory_bytes_{0};
  mutable std::mutex write_mutex_;
  mutable std::shared_mutex state_mutex_;
  std::shared_ptr<MemTable> active_;
  std::deque<std::shared_ptr<const MemTable>> immutable_tables_;
  std::uint64_t next_generation_{1};
  std::atomic<std::size_t> live_key_count_{0};
  std::atomic<std::uint64_t> last_sequence_number_{0};
};

} // namespace nebulakv
