#pragma once

#include "nebulakv/durability_mode.hpp"
#include "nebulakv/in_memory_key_value_store.hpp"
#include "nebulakv/key_value_store.hpp"
#include "nebulakv/recovery_manager.hpp"
#include "nebulakv/wal_writer.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace nebulakv {

struct PersistentStoreOptions {
  std::filesystem::path wal_path;
  DurabilityMode durability_mode{DurabilityMode::Sync};
  std::chrono::milliseconds batch_flush_interval{100};
  bool truncate_invalid_wal_tail{true};
  bool emit_recovery_diagnostics{true};
};

class PersistentKeyValueStore final : public KeyValueStore {
public:
  explicit PersistentKeyValueStore(PersistentStoreOptions options);
  ~PersistentKeyValueStore() override = default;

  PersistentKeyValueStore(const PersistentKeyValueStore&) = delete;
  PersistentKeyValueStore& operator=(const PersistentKeyValueStore&) = delete;
  PersistentKeyValueStore(PersistentKeyValueStore&&) = delete;
  PersistentKeyValueStore& operator=(PersistentKeyValueStore&&) = delete;

  void put(std::string key, std::string value) override;
  [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;
  bool remove(std::string_view key) override;
  [[nodiscard]] bool exists(std::string_view key) const override;

  void flush();

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] const RecoveryReport& recovery_report() const noexcept;
  [[nodiscard]] DurabilityMode durability_mode() const noexcept;

private:
  mutable std::mutex write_mutex_;
  InMemoryKeyValueStore memory_store_;
  RecoveryReport recovery_report_;
  std::unique_ptr<WalWriter> wal_writer_;
};

} // namespace nebulakv
