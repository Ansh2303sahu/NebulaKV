#include "nebulakv/network/grpc_service.hpp"

#include <chrono>
#include <cstddef>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nebulakv::network {

namespace {

[[nodiscard]] bool deadline_expired(const grpc::ServerContext& context) {
  return context.IsCancelled() || std::chrono::system_clock::now() >= context.deadline();
}

void fill_error(v1::ErrorDetail* target, const ApiError& error) {
  if (!error) {
    return;
  }
  target->set_code(std::string{to_string(error.code)});
  target->set_message(error.message);
}

[[nodiscard]] grpc::Status queue_full_status() {
  return {grpc::StatusCode::RESOURCE_EXHAUSTED, "server request queue is full"};
}

[[nodiscard]] grpc::Status deadline_status() {
  return {grpc::StatusCode::DEADLINE_EXCEEDED, "request deadline expired before completion"};
}

} // namespace

KeyValueServiceImpl::KeyValueServiceImpl(RequestProcessor& processor, BoundedExecutor& executor,
                                         GrpcServiceOptions options)
    : processor_{processor}, executor_{executor}, options_{options} {
  if (options_.max_request_bytes == 0U) {
    throw std::invalid_argument{"maximum request size must be greater than zero"};
  }
}

grpc::Status KeyValueServiceImpl::Put(grpc::ServerContext* context, const v1::PutRequest* request,
                                      v1::PutResponse* response) {
  requests_total_.fetch_add(1U, std::memory_order_relaxed);
  if (request_too_large(request->ByteSizeLong())) {
    record_failure();
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, "request exceeds the configured maximum size"};
  }
  if (deadline_expired(*context)) {
    record_failure();
    return deadline_status();
  }

  const auto deadline = context->deadline();
  auto future = executor_.try_submit(
      [this, deadline, key = request->key(), value = request->value()]() mutable {
        if (std::chrono::system_clock::now() >= deadline) {
          return PutResult{
              false,
              0U,
              {ApiErrorCode::DeadlineExceeded, "request expired while waiting for a worker"}};
        }
        return processor_.put(std::move(key), std::move(value));
      });
  if (!future) {
    record_failure();
    return queue_full_status();
  }
  if (future->wait_until(deadline) != std::future_status::ready) {
    record_failure();
    return deadline_status();
  }

  PutResult result = future->get();
  response->set_success(result.success);
  response->set_sequence_number(result.sequence_number);
  fill_error(response->mutable_error(), result.error);
  if (result.error) {
    record_failure();
    return status_from_error(result.error);
  }
  return grpc::Status::OK;
}

grpc::Status KeyValueServiceImpl::Get(grpc::ServerContext* context, const v1::GetRequest* request,
                                      v1::GetResponse* response) {
  requests_total_.fetch_add(1U, std::memory_order_relaxed);
  if (request_too_large(request->ByteSizeLong())) {
    record_failure();
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, "request exceeds the configured maximum size"};
  }
  if (deadline_expired(*context)) {
    record_failure();
    return deadline_status();
  }

  const auto deadline = context->deadline();
  auto future = executor_.try_submit([this, deadline, key = request->key()] {
    if (std::chrono::system_clock::now() >= deadline) {
      return GetResult{
          false,
          {},
          {ApiErrorCode::DeadlineExceeded, "request expired while waiting for a worker"}};
    }
    return processor_.get(key);
  });
  if (!future) {
    record_failure();
    return queue_full_status();
  }
  if (future->wait_until(deadline) != std::future_status::ready) {
    record_failure();
    return deadline_status();
  }

  GetResult result = future->get();
  response->set_found(result.found);
  if (result.found) {
    response->set_value(std::move(result.value));
  }
  fill_error(response->mutable_error(), result.error);
  if (result.error) {
    record_failure();
    return status_from_error(result.error);
  }
  return grpc::Status::OK;
}

grpc::Status KeyValueServiceImpl::Delete(grpc::ServerContext* context,
                                         const v1::DeleteRequest* request,
                                         v1::DeleteResponse* response) {
  requests_total_.fetch_add(1U, std::memory_order_relaxed);
  if (request_too_large(request->ByteSizeLong())) {
    record_failure();
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, "request exceeds the configured maximum size"};
  }
  if (deadline_expired(*context)) {
    record_failure();
    return deadline_status();
  }

  const auto deadline = context->deadline();
  auto future = executor_.try_submit([this, deadline, key = request->key()] {
    if (std::chrono::system_clock::now() >= deadline) {
      return DeleteResult{
          false, {ApiErrorCode::DeadlineExceeded, "request expired while waiting for a worker"}};
    }
    return processor_.remove(key);
  });
  if (!future) {
    record_failure();
    return queue_full_status();
  }
  if (future->wait_until(deadline) != std::future_status::ready) {
    record_failure();
    return deadline_status();
  }

  DeleteResult result = future->get();
  response->set_deleted(result.deleted);
  fill_error(response->mutable_error(), result.error);
  if (result.error) {
    record_failure();
    return status_from_error(result.error);
  }
  return grpc::Status::OK;
}

grpc::Status KeyValueServiceImpl::BatchPut(grpc::ServerContext* context,
                                           const v1::BatchPutRequest* request,
                                           v1::BatchPutResponse* response) {
  requests_total_.fetch_add(1U, std::memory_order_relaxed);
  if (request_too_large(request->ByteSizeLong())) {
    record_failure();
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, "request exceeds the configured maximum size"};
  }
  if (deadline_expired(*context)) {
    record_failure();
    return deadline_status();
  }

  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(static_cast<std::size_t>(request->entries_size()));
  for (const auto& entry : request->entries()) {
    entries.emplace_back(entry.key(), entry.value());
  }

  const auto deadline = context->deadline();
  auto future = executor_.try_submit([this, deadline, entries = std::move(entries)]() mutable {
    if (std::chrono::system_clock::now() >= deadline) {
      return BatchPutResult{
          false,
          0U,
          0U,
          {ApiErrorCode::DeadlineExceeded, "request expired while waiting for a worker"}};
    }
    return processor_.batch_put(std::move(entries));
  });
  if (!future) {
    record_failure();
    return queue_full_status();
  }
  if (future->wait_until(deadline) != std::future_status::ready) {
    record_failure();
    return deadline_status();
  }

  BatchPutResult result = future->get();
  response->set_success(result.success);
  response->set_writes_applied(result.writes_applied);
  response->set_last_sequence_number(result.last_sequence_number);
  fill_error(response->mutable_error(), result.error);
  if (result.error) {
    record_failure();
    return status_from_error(result.error);
  }
  return grpc::Status::OK;
}

grpc::Status KeyValueServiceImpl::Status(grpc::ServerContext* context,
                                         const v1::StatusRequest* request,
                                         v1::StatusResponse* response) {
  static_cast<void>(request);
  requests_total_.fetch_add(1U, std::memory_order_relaxed);
  if (deadline_expired(*context)) {
    record_failure();
    return deadline_status();
  }

  const StatusSnapshot snapshot = processor_.status(executor_.statistics(), statistics());
  response->set_ready(snapshot.ready);
  response->set_live_keys(snapshot.live_keys);
  response->set_last_sequence_number(snapshot.last_sequence_number);
  response->set_level0_sstables(snapshot.level0_sstables);
  response->set_level1_sstables(snapshot.level1_sstables);
  response->set_cache_hits(snapshot.cache_hits);
  response->set_cache_misses(snapshot.cache_misses);
  response->set_cache_evictions(snapshot.cache_evictions);
  response->set_cache_hit_ratio(snapshot.cache_hit_ratio);
  response->set_compactions_completed(snapshot.compactions_completed);
  response->set_queued_requests(snapshot.queued_requests);
  response->set_active_requests(snapshot.active_requests);
  response->set_rejected_requests(snapshot.rejected_requests);
  response->set_requests_total(snapshot.requests_total);
  response->set_failed_requests_total(snapshot.failed_requests_total);
  return grpc::Status::OK;
}

ServiceStatistics KeyValueServiceImpl::statistics() const noexcept {
  return {requests_total_.load(std::memory_order_relaxed),
          failed_requests_total_.load(std::memory_order_relaxed)};
}

bool KeyValueServiceImpl::request_too_large(const std::size_t encoded_size) const noexcept {
  return encoded_size > options_.max_request_bytes;
}

grpc::Status KeyValueServiceImpl::status_from_error(const ApiError& error) const {
  switch (error.code) {
  case ApiErrorCode::None:
    return grpc::Status::OK;
  case ApiErrorCode::InvalidArgument:
    return {grpc::StatusCode::INVALID_ARGUMENT, error.message};
  case ApiErrorCode::ResourceExhausted:
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, error.message};
  case ApiErrorCode::DeadlineExceeded:
    return {grpc::StatusCode::DEADLINE_EXCEEDED, error.message};
  case ApiErrorCode::Unavailable:
    return {grpc::StatusCode::UNAVAILABLE, error.message};
  case ApiErrorCode::Internal:
    return {grpc::StatusCode::INTERNAL, error.message};
  }
  return {grpc::StatusCode::INTERNAL, "unknown service error"};
}

void KeyValueServiceImpl::record_failure() noexcept {
  failed_requests_total_.fetch_add(1U, std::memory_order_relaxed);
}

} // namespace nebulakv::network
