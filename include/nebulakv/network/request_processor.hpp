#pragma once

#include "nebulakv/network/bounded_executor.hpp"
#include "nebulakv/persistent_key_value_store.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nebulakv::network {

enum class ApiErrorCode {
  None,
  InvalidArgument,
  ResourceExhausted,
  DeadlineExceeded,
  Unavailable,
  Internal,
};

struct ApiError {
  ApiErrorCode code{ApiErrorCode::None};
  std::string message;

  [[nodiscard]] explicit operator bool() const noexcept { return code != ApiErrorCode::None; }
};

struct PutResult {
  bool success{false};
  std::uint64_t sequence_number{0};
  ApiError error;
};

struct GetResult {
  bool found{false};
  std::string value;
  ApiError error;
};

struct DeleteResult {
  bool deleted{false};
  ApiError error;
};

struct BatchPutResult {
  bool success{false};
  std::uint32_t writes_applied{0};
  std::uint64_t last_sequence_number{0};
  ApiError error;
};

struct ServiceStatistics {
  std::uint64_t requests_total{0};
  std::uint64_t failed_requests_total{0};
};

struct StatusSnapshot {
  bool ready{true};
  std::uint64_t live_keys{0};
  std::uint64_t last_sequence_number{0};
  std::uint64_t level0_sstables{0};
  std::uint64_t level1_sstables{0};
  std::uint64_t cache_hits{0};
  std::uint64_t cache_misses{0};
  std::uint64_t cache_evictions{0};
  double cache_hit_ratio{0.0};
  std::uint64_t compactions_completed{0};
  std::uint64_t queued_requests{0};
  std::uint64_t active_requests{0};
  std::uint64_t rejected_requests{0};
  std::uint64_t requests_total{0};
  std::uint64_t failed_requests_total{0};
};

struct RequestProcessorOptions {
  std::size_t max_batch_entries{1024U};
  std::size_t max_batch_bytes{8U * 1024U * 1024U};
};

class RequestProcessor final {
public:
  RequestProcessor(PersistentKeyValueStore& store, RequestProcessorOptions options = {});

  [[nodiscard]] PutResult put(std::string key, std::string value) noexcept;
  [[nodiscard]] GetResult get(std::string_view key) const noexcept;
  [[nodiscard]] DeleteResult remove(std::string_view key) noexcept;
  [[nodiscard]] BatchPutResult
  batch_put(std::vector<std::pair<std::string, std::string>> entries) noexcept;
  [[nodiscard]] StatusSnapshot status(const ExecutorStatistics& executor,
                                      const ServiceStatistics& service) const noexcept;

private:
  [[nodiscard]] ApiError
  validate_batch(const std::vector<std::pair<std::string, std::string>>& entries) const noexcept;

  PersistentKeyValueStore& store_;
  RequestProcessorOptions options_;
};

[[nodiscard]] std::string_view to_string(ApiErrorCode code) noexcept;

} // namespace nebulakv::network
