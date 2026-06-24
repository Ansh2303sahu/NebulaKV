#pragma once

#include "nebulakv/durability_mode.hpp"
#include "nebulakv/wal_record.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <mutex>
#include <thread>

namespace nebulakv {

struct WalWriterOptions {
  std::filesystem::path path;
  DurabilityMode durability_mode{DurabilityMode::Sync};
  std::chrono::milliseconds batch_flush_interval{100};
};

class WalWriter final {
public:
  explicit WalWriter(WalWriterOptions options);
  ~WalWriter();

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;
  WalWriter(WalWriter&&) = delete;
  WalWriter& operator=(WalWriter&&) = delete;

  void append(const WalRecord& record);
  void flush();
  void reset();

  [[nodiscard]] std::uint64_t bytes_appended() const;
  [[nodiscard]] DurabilityMode durability_mode() const noexcept;

private:
  void batch_flush_loop();
  void flush_locked();
  void throw_if_background_flush_failed_locked() const;

  int file_descriptor_{-1};
  DurabilityMode durability_mode_{DurabilityMode::Sync};
  std::chrono::milliseconds batch_flush_interval_{100};
  mutable std::mutex mutex_;
  std::condition_variable flush_condition_;
  std::thread flush_thread_;
  bool dirty_{false};
  bool stopping_{false};
  std::exception_ptr background_flush_error_;
  std::uint64_t bytes_appended_{0};
};

} // namespace nebulakv
