#include "nebulakv/network/request_processor.hpp"

#include "nebulakv/validation.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nebulakv::network {

namespace {

[[nodiscard]] ApiError map_exception(const std::exception& error) noexcept {
  if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr ||
      dynamic_cast<const std::length_error*>(&error) != nullptr) {
    return {ApiErrorCode::InvalidArgument, error.what()};
  }
  return {ApiErrorCode::Internal, error.what()};
}

} // namespace

RequestProcessor::RequestProcessor(PersistentKeyValueStore& store, RequestProcessorOptions options)
    : store_{store}, options_{options} {
  if (options_.max_batch_entries == 0U) {
    throw std::invalid_argument{"maximum batch entries must be greater than zero"};
  }
  if (options_.max_batch_bytes == 0U) {
    throw std::invalid_argument{"maximum batch bytes must be greater than zero"};
  }
}

PutResult RequestProcessor::put(std::string key, std::string value) noexcept {
  try {
    store_.put(std::move(key), std::move(value));
    return {true, store_.last_sequence_number(), {}};
  } catch (const std::exception& error) {
    return {false, store_.last_sequence_number(), map_exception(error)};
  } catch (...) {
    return {
        false, store_.last_sequence_number(), {ApiErrorCode::Internal, "unknown storage error"}};
  }
}

GetResult RequestProcessor::get(const std::string_view key) const noexcept {
  try {
    auto value = store_.get(key);
    if (!value) {
      return {};
    }
    return {true, std::move(*value), {}};
  } catch (const std::exception& error) {
    return {false, {}, map_exception(error)};
  } catch (...) {
    return {false, {}, {ApiErrorCode::Internal, "unknown storage error"}};
  }
}

DeleteResult RequestProcessor::remove(const std::string_view key) noexcept {
  try {
    return {store_.remove(key), {}};
  } catch (const std::exception& error) {
    return {false, map_exception(error)};
  } catch (...) {
    return {false, {ApiErrorCode::Internal, "unknown storage error"}};
  }
}

BatchPutResult
RequestProcessor::batch_put(std::vector<std::pair<std::string, std::string>> entries) noexcept {
  if (const ApiError validation_error = validate_batch(entries)) {
    return {false, 0U, store_.last_sequence_number(), validation_error};
  }

  std::uint32_t writes_applied = 0;
  for (auto& [key, value] : entries) {
    PutResult result = put(std::move(key), std::move(value));
    if (!result.success) {
      return {false, writes_applied, store_.last_sequence_number(), std::move(result.error)};
    }
    ++writes_applied;
  }

  return {true, writes_applied, store_.last_sequence_number(), {}};
}

StatusSnapshot RequestProcessor::status(const ExecutorStatistics& executor,
                                        const ServiceStatistics& service) const noexcept {
  const auto cache = store_.block_cache_statistics();
  const auto compaction = store_.compaction_statistics();

  StatusSnapshot snapshot;
  snapshot.live_keys = static_cast<std::uint64_t>(store_.size());
  snapshot.last_sequence_number = store_.last_sequence_number();
  snapshot.level0_sstables = static_cast<std::uint64_t>(store_.level0_sstable_count());
  snapshot.level1_sstables = static_cast<std::uint64_t>(store_.level1_sstable_count());
  snapshot.cache_hits = cache.hits;
  snapshot.cache_misses = cache.misses;
  snapshot.cache_evictions = cache.evictions;
  snapshot.cache_hit_ratio = cache.hit_ratio();
  snapshot.compactions_completed = compaction.runs;
  snapshot.queued_requests = static_cast<std::uint64_t>(executor.queued_tasks);
  snapshot.active_requests = static_cast<std::uint64_t>(executor.active_tasks);
  snapshot.rejected_requests = executor.rejected_tasks;
  snapshot.requests_total = service.requests_total;
  snapshot.failed_requests_total = service.failed_requests_total;
  return snapshot;
}

ApiError RequestProcessor::validate_batch(
    const std::vector<std::pair<std::string, std::string>>& entries) const noexcept {
  if (entries.empty()) {
    return {ApiErrorCode::InvalidArgument, "batch must contain at least one entry"};
  }
  if (entries.size() > options_.max_batch_entries) {
    return {ApiErrorCode::InvalidArgument, "batch exceeds the maximum number of entries"};
  }

  std::size_t total_bytes = 0;
  try {
    for (const auto& [key, value] : entries) {
      validate_key(key);
      validate_value(value);
      if (key.size() > std::numeric_limits<std::size_t>::max() - total_bytes ||
          value.size() > std::numeric_limits<std::size_t>::max() - total_bytes - key.size()) {
        return {ApiErrorCode::InvalidArgument, "batch byte size overflow"};
      }
      total_bytes += key.size() + value.size();
      if (total_bytes > options_.max_batch_bytes) {
        return {ApiErrorCode::InvalidArgument, "batch exceeds the maximum encoded payload size"};
      }
    }
  } catch (const std::exception& error) {
    return map_exception(error);
  } catch (...) {
    return {ApiErrorCode::Internal, "unknown validation error"};
  }

  return {};
}

std::string_view to_string(const ApiErrorCode code) noexcept {
  switch (code) {
  case ApiErrorCode::None:
    return "NONE";
  case ApiErrorCode::InvalidArgument:
    return "INVALID_ARGUMENT";
  case ApiErrorCode::ResourceExhausted:
    return "RESOURCE_EXHAUSTED";
  case ApiErrorCode::DeadlineExceeded:
    return "DEADLINE_EXCEEDED";
  case ApiErrorCode::Unavailable:
    return "UNAVAILABLE";
  case ApiErrorCode::Internal:
    return "INTERNAL";
  }
  return "INTERNAL";
}

} // namespace nebulakv::network
