#pragma once

#include "nebulakv/raft/types.hpp"

#include <filesystem>
#include <mutex>
#include <vector>

namespace nebulakv::raft {

class RaftStorage final {
public:
  explicit RaftStorage(std::filesystem::path directory);

  [[nodiscard]] PersistentState load() const;
  void save_hard_state(const HardState& state);
  void replace_log(const std::vector<LogEntry>& entries);
  void save_snapshot(const Snapshot& snapshot);

  [[nodiscard]] const std::filesystem::path& directory() const noexcept;
  [[nodiscard]] const std::filesystem::path& hard_state_path() const noexcept;
  [[nodiscard]] const std::filesystem::path& log_path() const noexcept;
  [[nodiscard]] const std::filesystem::path& snapshot_path() const noexcept;

private:
  std::filesystem::path directory_;
  std::filesystem::path hard_state_path_;
  std::filesystem::path log_path_;
  std::filesystem::path snapshot_path_;
  mutable std::mutex mutex_;
};

} // namespace nebulakv::raft
