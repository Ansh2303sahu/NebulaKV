#pragma once

#include "nebulakv/durability_mode.hpp"
#include "nebulakv/persistent_key_value_store.hpp"
#include "nebulakv/raft/types.hpp"

#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nebulakv::raft {

struct StateMachineStatistics {
  std::uint64_t last_sequence_number{0};
  std::size_t level0_sstables{0};
  std::size_t level1_sstables{0};
  std::uint64_t cache_hits{0};
  std::uint64_t cache_misses{0};
  std::uint64_t cache_evictions{0};
  double cache_hit_ratio{0.0};
  std::uint64_t compactions_completed{0};
};

class StateMachine {
public:
  virtual ~StateMachine() = default;

  virtual void recover(const std::optional<Snapshot>& snapshot,
                       const std::vector<LogEntry>& committed_entries) = 0;
  virtual void apply(const LogEntry& entry) = 0;
  [[nodiscard]] virtual std::optional<std::string> get(std::string_view key) const = 0;
  [[nodiscard]] virtual std::string create_snapshot() const = 0;
  virtual void install_snapshot(std::string_view payload) = 0;
  virtual void flush() = 0;
  [[nodiscard]] virtual std::size_t size() const noexcept = 0;
  [[nodiscard]] virtual StateMachineStatistics statistics() const = 0;
};

struct DurableStateMachineOptions {
  std::filesystem::path directory;
  DurabilityMode durability_mode{DurabilityMode::Sync};
  std::size_t memtable_max_bytes{64U * 1024U * 1024U};
  std::size_t block_cache_capacity_bytes{64U * 1024U * 1024U};
};

class DurableKeyValueStateMachine final : public StateMachine {
public:
  explicit DurableKeyValueStateMachine(DurableStateMachineOptions options);
  ~DurableKeyValueStateMachine() override;

  void recover(const std::optional<Snapshot>& snapshot,
               const std::vector<LogEntry>& committed_entries) override;
  void apply(const LogEntry& entry) override;
  [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;
  [[nodiscard]] std::string create_snapshot() const override;
  void install_snapshot(std::string_view payload) override;
  void flush() override;
  [[nodiscard]] std::size_t size() const noexcept override;
  [[nodiscard]] StateMachineStatistics statistics() const override;

  [[nodiscard]] PersistentKeyValueStore& storage();
  [[nodiscard]] const PersistentKeyValueStore& storage() const;
  [[nodiscard]] const std::filesystem::path& directory() const noexcept;

private:
  void reset_storage_locked();
  void open_storage_locked();
  void apply_command_locked(const Command& command);
  void restore_map_locked(std::string_view payload);
  [[nodiscard]] std::string encode_map_locked() const;

  DurableStateMachineOptions options_;
  mutable std::mutex mutex_;
  std::map<std::string, std::string, std::less<>> values_;
  std::unique_ptr<PersistentKeyValueStore> store_;
};

} // namespace nebulakv::raft
