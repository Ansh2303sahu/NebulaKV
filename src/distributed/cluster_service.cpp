#include "nebulakv/distributed/cluster_service.hpp"

#include "nebulakv/validation.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nebulakv::distributed {

namespace {

[[nodiscard]] grpc::Status queue_full_status() {
  return {grpc::StatusCode::RESOURCE_EXHAUSTED, "server request queue is full"};
}

[[nodiscard]] grpc::Status deadline_status() {
  return {grpc::StatusCode::DEADLINE_EXCEEDED, "request deadline expired before completion"};
}

[[nodiscard]] bool deadline_expired(const grpc::ServerContext& context) {
  return context.IsCancelled() || std::chrono::system_clock::now() >= context.deadline();
}

struct ReadResult {
  std::optional<std::string> value;
  std::string error;
  bool quorum_confirmed{false};
};

struct DeleteResult {
  raft::SubmitResult submit;
  bool existed{false};
  std::string error;
  bool quorum_confirmed{false};
};

} // namespace

ClusterKeyValueServiceImpl::ClusterKeyValueServiceImpl(raft::RaftNode& node,
                                                       raft::StateMachine& state_machine,
                                                       network::BoundedExecutor& executor,
                                                       observability::MetricsRegistry& metrics,
                                                       observability::JsonLogger& logger,
                                                       ClusterServiceOptions options)
    : node_{node}, state_machine_{state_machine}, executor_{executor}, metrics_{metrics},
      logger_{logger}, options_{std::move(options)} {
  if (options_.advertised_address.empty()) {
    throw std::invalid_argument{"advertised cluster address must not be empty"};
  }
  if (options_.max_request_bytes == 0U || options_.max_batch_entries == 0U ||
      options_.max_batch_bytes == 0U) {
    throw std::invalid_argument{"cluster service limits must be positive"};
  }
}

grpc::Status ClusterKeyValueServiceImpl::Put(grpc::ServerContext* context,
                                             const v1::PutRequest* request,
                                             v1::PutResponse* response) {
  const auto started = std::chrono::steady_clock::now();
  record_request("put");
  if (request->ByteSizeLong() > options_.max_request_bytes) {
    record_failure("put");
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, "request exceeds the configured maximum size"};
  }
  if (deadline_expired(*context)) {
    record_failure("put");
    return deadline_status();
  }

  const auto deadline = steady_deadline(*context);
  auto future = executor_.try_submit(
      [this, deadline, key = request->key(), value = request->value()]() mutable {
        return node_.submit({raft::CommandType::Put, std::move(key), std::move(value)}, deadline);
      });
  if (!future) {
    record_failure("put");
    return queue_full_status();
  }
  if (future->wait_until(context->deadline()) != std::future_status::ready) {
    record_failure("put");
    return deadline_status();
  }

  const raft::SubmitResult result = future->get();
  response->set_success(result.committed());
  response->set_sequence_number(result.log_index);
  const grpc::Status status = submit_status(*context, result, response->mutable_error());
  if (!status.ok()) {
    record_failure("put");
  }
  record_latency("put", started);
  refresh_raft_metrics();
  return status;
}

grpc::Status ClusterKeyValueServiceImpl::Get(grpc::ServerContext* context,
                                             const v1::GetRequest* request,
                                             v1::GetResponse* response) {
  const auto started = std::chrono::steady_clock::now();
  record_request("get");
  if (request->ByteSizeLong() > options_.max_request_bytes) {
    record_failure("get");
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, "request exceeds the configured maximum size"};
  }
  try {
    validate_key(request->key());
  } catch (const std::exception& error) {
    record_failure("get");
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  }
  if (!node_.is_leader()) {
    record_failure("get");
    record_latency("get", started);
    return not_leader(*context, response->mutable_error());
  }
  if (deadline_expired(*context)) {
    record_failure("get");
    return deadline_status();
  }

  const auto deadline = steady_deadline(*context);
  auto future = executor_.try_submit([this, deadline, key = request->key()] {
    ReadResult result;
    result.quorum_confirmed = node_.confirm_leadership(deadline);
    if (!result.quorum_confirmed) {
      return result;
    }
    try {
      result.value = state_machine_.get(key);
    } catch (const std::exception& error) {
      result.error = error.what();
    }
    return result;
  });
  if (!future) {
    record_failure("get");
    return queue_full_status();
  }
  if (future->wait_until(context->deadline()) != std::future_status::ready) {
    record_failure("get");
    return deadline_status();
  }
  ReadResult result = future->get();
  if (!result.quorum_confirmed) {
    record_failure("get");
    record_latency("get", started);
    if (!node_.is_leader()) {
      return not_leader(*context, response->mutable_error());
    }
    response->mutable_error()->set_code("UNAVAILABLE");
    response->mutable_error()->set_message("leader could not confirm a current-term quorum");
    return {grpc::StatusCode::UNAVAILABLE, "leader could not confirm a current-term quorum"};
  }
  if (!result.error.empty()) {
    record_failure("get");
    response->mutable_error()->set_code("INTERNAL");
    response->mutable_error()->set_message(result.error);
    return {grpc::StatusCode::INTERNAL, result.error};
  }
  response->set_found(result.value.has_value());
  if (result.value) {
    response->set_value(std::move(*result.value));
  }
  record_latency("get", started);
  refresh_raft_metrics();
  return grpc::Status::OK;
}

grpc::Status ClusterKeyValueServiceImpl::Delete(grpc::ServerContext* context,
                                                const v1::DeleteRequest* request,
                                                v1::DeleteResponse* response) {
  const auto started = std::chrono::steady_clock::now();
  record_request("delete");
  if (request->ByteSizeLong() > options_.max_request_bytes) {
    record_failure("delete");
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, "request exceeds the configured maximum size"};
  }
  try {
    validate_key(request->key());
  } catch (const std::exception& error) {
    record_failure("delete");
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  }
  if (!node_.is_leader()) {
    record_failure("delete");
    record_latency("delete", started);
    return not_leader(*context, response->mutable_error());
  }
  if (deadline_expired(*context)) {
    record_failure("delete");
    return deadline_status();
  }
  const auto deadline = steady_deadline(*context);
  auto future = executor_.try_submit([this, deadline, key = request->key()]() mutable {
    DeleteResult result;
    result.quorum_confirmed = node_.confirm_leadership(deadline);
    if (!result.quorum_confirmed) {
      return result;
    }
    try {
      result.existed = state_machine_.get(key).has_value();
      result.submit = node_.submit({raft::CommandType::Delete, std::move(key), {}}, deadline);
    } catch (const std::exception& error) {
      result.error = error.what();
    }
    return result;
  });
  if (!future) {
    record_failure("delete");
    return queue_full_status();
  }
  if (future->wait_until(context->deadline()) != std::future_status::ready) {
    record_failure("delete");
    return deadline_status();
  }
  DeleteResult delete_result = future->get();
  if (!delete_result.quorum_confirmed) {
    record_failure("delete");
    record_latency("delete", started);
    if (!node_.is_leader()) {
      return not_leader(*context, response->mutable_error());
    }
    response->mutable_error()->set_code("UNAVAILABLE");
    response->mutable_error()->set_message("leader could not confirm a current-term quorum");
    return {grpc::StatusCode::UNAVAILABLE, "leader could not confirm a current-term quorum"};
  }
  if (!delete_result.error.empty()) {
    record_failure("delete");
    response->mutable_error()->set_code("INTERNAL");
    response->mutable_error()->set_message(delete_result.error);
    return {grpc::StatusCode::INTERNAL, delete_result.error};
  }
  response->set_deleted(delete_result.submit.committed() && delete_result.existed);
  const grpc::Status status =
      submit_status(*context, delete_result.submit, response->mutable_error());
  if (!status.ok()) {
    record_failure("delete");
  }
  record_latency("delete", started);
  refresh_raft_metrics();
  return status;
}

grpc::Status ClusterKeyValueServiceImpl::BatchPut(grpc::ServerContext* context,
                                                  const v1::BatchPutRequest* request,
                                                  v1::BatchPutResponse* response) {
  const auto started = std::chrono::steady_clock::now();
  record_request("batch_put");
  if (request->ByteSizeLong() > options_.max_request_bytes) {
    record_failure("batch_put");
    return {grpc::StatusCode::RESOURCE_EXHAUSTED, "request exceeds the configured maximum size"};
  }
  if (request->entries_size() <= 0 ||
      static_cast<std::size_t>(request->entries_size()) > options_.max_batch_entries) {
    record_failure("batch_put");
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "batch entry count is outside the configured range"};
  }

  std::vector<std::pair<std::string, std::string>> entries;
  entries.reserve(static_cast<std::size_t>(request->entries_size()));
  std::size_t total_bytes = 0U;
  try {
    for (const auto& entry : request->entries()) {
      validate_key(entry.key());
      validate_value(entry.value());
      if (entry.key().size() > std::numeric_limits<std::size_t>::max() - total_bytes ||
          entry.value().size() >
              std::numeric_limits<std::size_t>::max() - total_bytes - entry.key().size()) {
        throw std::length_error{"batch byte size overflow"};
      }
      total_bytes += entry.key().size() + entry.value().size();
      if (total_bytes > options_.max_batch_bytes) {
        throw std::length_error{"batch exceeds maximum payload bytes"};
      }
      entries.emplace_back(entry.key(), entry.value());
    }
  } catch (const std::exception& error) {
    record_failure("batch_put");
    return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
  }

  const auto deadline = steady_deadline(*context);
  auto future = executor_.try_submit([this, deadline, entries = std::move(entries)]() mutable {
    std::vector<raft::SubmitResult> results;
    results.reserve(entries.size());
    for (auto& [key, value] : entries) {
      results.push_back(
          node_.submit({raft::CommandType::Put, std::move(key), std::move(value)}, deadline));
      if (!results.back().committed()) {
        break;
      }
    }
    return results;
  });
  if (!future) {
    record_failure("batch_put");
    return queue_full_status();
  }
  if (future->wait_until(context->deadline()) != std::future_status::ready) {
    record_failure("batch_put");
    return deadline_status();
  }

  const auto results = future->get();
  std::uint32_t applied = 0U;
  for (const auto& result : results) {
    if (!result.committed()) {
      break;
    }
    ++applied;
    response->set_last_sequence_number(result.log_index);
  }
  response->set_writes_applied(applied);
  response->set_success(applied == static_cast<std::uint32_t>(request->entries_size()));
  grpc::Status status = grpc::Status::OK;
  if (!response->success()) {
    const raft::SubmitResult failure = results.empty() ? raft::SubmitResult{} : results.back();
    status = submit_status(*context, failure, response->mutable_error());
    record_failure("batch_put");
  }
  record_latency("batch_put", started);
  refresh_raft_metrics();
  return status;
}

grpc::Status ClusterKeyValueServiceImpl::Status(grpc::ServerContext* context,
                                                const v1::StatusRequest* request,
                                                v1::StatusResponse* response) {
  static_cast<void>(request);
  record_request("status");
  if (deadline_expired(*context)) {
    record_failure("status");
    return deadline_status();
  }

  const auto node_status = node_.status();
  const auto state_status = state_machine_.statistics();
  const auto executor_status = executor_.statistics();
  const auto service_status = statistics();
  response->set_ready(true);
  response->set_live_keys(static_cast<std::uint64_t>(state_machine_.size()));
  response->set_last_sequence_number(state_status.last_sequence_number);
  response->set_level0_sstables(static_cast<std::uint64_t>(state_status.level0_sstables));
  response->set_level1_sstables(static_cast<std::uint64_t>(state_status.level1_sstables));
  response->set_cache_hits(state_status.cache_hits);
  response->set_cache_misses(state_status.cache_misses);
  response->set_cache_evictions(state_status.cache_evictions);
  response->set_cache_hit_ratio(state_status.cache_hit_ratio);
  response->set_compactions_completed(state_status.compactions_completed);
  response->set_queued_requests(static_cast<std::uint64_t>(executor_status.queued_tasks));
  response->set_active_requests(static_cast<std::uint64_t>(executor_status.active_tasks));
  response->set_rejected_requests(executor_status.rejected_tasks);
  response->set_requests_total(service_status.requests_total);
  response->set_failed_requests_total(service_status.failed_requests_total);
  response->set_node_id(node_status.node_id);
  response->set_raft_role(std::string{raft::to_string(node_status.role)});
  response->set_raft_term(node_status.term);
  response->set_leader_id(node_status.leader_id);
  if (node_status.leader_id == node_status.node_id) {
    response->set_leader_address(options_.advertised_address);
  } else if (const auto address = node_.leader_address()) {
    response->set_leader_address(*address);
  }
  response->set_raft_commit_index(node_status.commit_index);
  response->set_raft_last_applied(node_status.last_applied);
  response->set_raft_last_log_index(node_status.last_log_index);
  response->set_raft_snapshot_index(node_status.snapshot_index);
  response->set_raft_elections_total(node_status.metrics.elections_started);
  response->set_raft_replication_failures_total(node_status.metrics.replication_failures);
  response->set_raft_snapshots_created_total(node_status.metrics.snapshots_created);
  response->set_raft_snapshots_installed_total(node_status.metrics.snapshots_installed);
  refresh_raft_metrics();
  return grpc::Status::OK;
}

ClusterServiceStatistics ClusterKeyValueServiceImpl::statistics() const noexcept {
  return {requests_total_.load(std::memory_order_relaxed),
          failed_requests_total_.load(std::memory_order_relaxed),
          redirects_total_.load(std::memory_order_relaxed)};
}

grpc::Status ClusterKeyValueServiceImpl::not_leader(grpc::ServerContext& context,
                                                    v1::ErrorDetail* error) {
  const auto status = node_.status();
  std::string leader_address;
  if (status.leader_id == status.node_id) {
    leader_address = options_.advertised_address;
  } else if (const auto address = node_.leader_address()) {
    leader_address = *address;
  }
  error->set_code("NOT_LEADER");
  error->set_message("request must be sent to the current Raft leader");
  error->set_leader_id(status.leader_id);
  error->set_leader_address(leader_address);
  if (!status.leader_id.empty()) {
    context.AddTrailingMetadata("nebulakv-leader-id", status.leader_id);
  }
  if (!leader_address.empty()) {
    context.AddTrailingMetadata("nebulakv-leader-address", leader_address);
  }
  redirects_total_.fetch_add(1U, std::memory_order_relaxed);
  metrics_.increment("nebulakv_redirects_total");
  logger_.log("info", "client_redirect",
              {{"node_id", status.node_id},
               {"leader_id", status.leader_id},
               {"leader_address", leader_address}});
  return {grpc::StatusCode::FAILED_PRECONDITION, "request was sent to a follower"};
}

grpc::Status ClusterKeyValueServiceImpl::submit_status(grpc::ServerContext& context,
                                                       const raft::SubmitResult& result,
                                                       v1::ErrorDetail* error) {
  switch (result.status) {
  case raft::SubmitStatus::Committed:
    return grpc::Status::OK;
  case raft::SubmitStatus::NotLeader:
    return not_leader(context, error);
  case raft::SubmitStatus::Timeout:
    error->set_code("DEADLINE_EXCEEDED");
    error->set_message(result.message);
    return {grpc::StatusCode::DEADLINE_EXCEEDED, result.message};
  case raft::SubmitStatus::Unavailable:
    error->set_code("UNAVAILABLE");
    error->set_message(result.message);
    return {grpc::StatusCode::UNAVAILABLE, result.message};
  case raft::SubmitStatus::InvalidCommand:
    error->set_code("INVALID_ARGUMENT");
    error->set_message(result.message);
    return {grpc::StatusCode::INVALID_ARGUMENT, result.message};
  }
  return {grpc::StatusCode::INTERNAL, "unknown Raft submit status"};
}

std::chrono::steady_clock::time_point
ClusterKeyValueServiceImpl::steady_deadline(const grpc::ServerContext& context) const {
  const auto remaining = context.deadline() - std::chrono::system_clock::now();
  if (remaining <= std::chrono::system_clock::duration::zero()) {
    return std::chrono::steady_clock::now();
  }
  return std::chrono::steady_clock::now() +
         std::chrono::duration_cast<std::chrono::steady_clock::duration>(remaining);
}

void ClusterKeyValueServiceImpl::record_request(const std::string_view operation) {
  requests_total_.fetch_add(1U, std::memory_order_relaxed);
  metrics_.increment("nebulakv_requests_total");
  metrics_.increment("nebulakv_" + std::string{operation} + "_requests_total");
}

void ClusterKeyValueServiceImpl::record_failure(const std::string_view operation) {
  failed_requests_total_.fetch_add(1U, std::memory_order_relaxed);
  metrics_.increment("nebulakv_request_failures_total");
  metrics_.increment("nebulakv_" + std::string{operation} + "_failures_total");
}

void ClusterKeyValueServiceImpl::record_latency(
    const std::string_view operation, const std::chrono::steady_clock::time_point started) {
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  metrics_.observe("nebulakv_request_latency_seconds", seconds);
  metrics_.observe("nebulakv_" + std::string{operation} + "_latency_seconds", seconds);
}

void ClusterKeyValueServiceImpl::refresh_raft_metrics() {
  const auto status = node_.status();
  metrics_.set_gauge("nebulakv_raft_term", static_cast<double>(status.term));
  metrics_.set_gauge("nebulakv_raft_role",
                     static_cast<double>(static_cast<std::uint8_t>(status.role)));
  metrics_.set_gauge("nebulakv_raft_commit_index", static_cast<double>(status.commit_index));
  metrics_.set_gauge("nebulakv_raft_last_applied", static_cast<double>(status.last_applied));
  metrics_.set_gauge("nebulakv_raft_replication_lag",
                     static_cast<double>(status.last_log_index - status.commit_index));
  metrics_.set_gauge("nebulakv_raft_elections_total",
                     static_cast<double>(status.metrics.elections_started));
  metrics_.set_gauge("nebulakv_raft_replication_failures_total",
                     static_cast<double>(status.metrics.replication_failures));
  metrics_.set_gauge("nebulakv_snapshot_created_total",
                     static_cast<double>(status.metrics.snapshots_created));
  metrics_.set_gauge("nebulakv_snapshot_installed_total",
                     static_cast<double>(status.metrics.snapshots_installed));
}

} // namespace nebulakv::distributed
