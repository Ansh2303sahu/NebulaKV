#include "nebulakv/distributed/cluster_server.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace nebulakv::distributed {

namespace {

[[nodiscard]] int checked_message_size(const std::size_t bytes) {
  if (bytes == 0U || bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument{"maximum cluster message size is invalid"};
  }
  return static_cast<int>(bytes);
}

} // namespace

ClusterServer::ClusterServer(raft::RaftNode& node, raft::StateMachine& state_machine,
                             observability::JsonLogger& logger, ClusterServerOptions options)
    : node_{node}, state_machine_{state_machine}, logger_{logger}, options_{std::move(options)},
      executor_{options_.worker_threads, options_.request_queue_capacity},
      key_value_service_{
          node_,
          state_machine_,
          executor_,
          metrics_,
          logger_,
          ClusterServiceOptions{options_.advertised_address, options_.max_message_bytes,
                                options_.max_batch_entries, options_.max_batch_bytes}},
      raft_service_{node_}, metrics_server_{options_.metrics_host, options_.metrics_port,
                                            [this] { return metrics_text(); }} {
  if (options_.listen_address.empty() || options_.advertised_address.empty()) {
    throw std::invalid_argument{"cluster listen and advertised addresses are required"};
  }
  static_cast<void>(checked_message_size(options_.max_message_bytes));
}

ClusterServer::~ClusterServer() {
  try {
    bool requires_shutdown = false;
    {
      std::lock_guard lock{lifecycle_mutex_};
      requires_shutdown = server_ != nullptr && !shutdown_finished_;
    }
    if (requires_shutdown) {
      request_shutdown(std::chrono::milliseconds{0});
      server_->Wait();
      finish_shutdown();
    }
  } catch (...) {
  }
}

void ClusterServer::start() {
  std::lock_guard lock{lifecycle_mutex_};
  if (started_) {
    throw std::logic_error{"cluster server has already been started"};
  }
  grpc::ServerBuilder builder;
  const int maximum = checked_message_size(options_.max_message_bytes);
  builder.SetMaxReceiveMessageSize(maximum);
  builder.SetMaxSendMessageSize(maximum);
  builder.AddListeningPort(options_.listen_address, grpc::InsecureServerCredentials(),
                           &bound_port_);
  builder.RegisterService(&key_value_service_);
  builder.RegisterService(&raft_service_);
  server_ = builder.BuildAndStart();
  if (!server_ || bound_port_ == 0) {
    throw std::runtime_error{"failed to bind clustered gRPC server"};
  }
  metrics_server_.start();
  node_.start();
  started_ = true;
  logger_.log("info", "cluster_server_started",
              {{"listen_address", options_.listen_address},
               {"advertised_address", options_.advertised_address},
               {"metrics_port", std::to_string(metrics_server_.bound_port())}});
}

void ClusterServer::wait() {
  {
    std::lock_guard lock{lifecycle_mutex_};
    if (!started_ || !server_) {
      throw std::logic_error{"cluster server is not running"};
    }
  }
  server_->Wait();
  finish_shutdown();
}

void ClusterServer::request_shutdown(const std::chrono::milliseconds grace_period) {
  std::lock_guard lock{lifecycle_mutex_};
  if (!server_ || shutdown_requested_) {
    return;
  }
  shutdown_requested_ = true;
  server_->Shutdown(std::chrono::system_clock::now() + grace_period);
}

int ClusterServer::bound_port() const noexcept { return bound_port_; }

std::uint16_t ClusterServer::metrics_port() const noexcept { return metrics_server_.bound_port(); }

ClusterServiceStatistics ClusterServer::service_statistics() const noexcept {
  return key_value_service_.statistics();
}

network::ExecutorStatistics ClusterServer::executor_statistics() const {
  return executor_.statistics();
}

std::string ClusterServer::metrics_text() {
  refresh_metrics();
  return metrics_.render_prometheus();
}

void ClusterServer::finish_shutdown() {
  {
    std::lock_guard lock{lifecycle_mutex_};
    if (shutdown_finished_) {
      return;
    }
    shutdown_finished_ = true;
  }
  node_.stop();
  executor_.shutdown(true);
  metrics_server_.stop();
  state_machine_.flush();
  logger_.log("info", "cluster_server_stopped",
              {{"requests_total", std::to_string(service_statistics().requests_total)}});
}

void ClusterServer::refresh_metrics() {
  const auto node_status = node_.status();
  const auto storage_status = state_machine_.statistics();
  const auto executor_status = executor_.statistics();
  metrics_.set_gauge("nebulakv_live_keys", static_cast<double>(state_machine_.size()));
  metrics_.set_gauge("nebulakv_raft_term", static_cast<double>(node_status.term));
  metrics_.set_gauge("nebulakv_raft_role",
                     static_cast<double>(static_cast<std::uint8_t>(node_status.role)));
  metrics_.set_gauge("nebulakv_raft_commit_index", static_cast<double>(node_status.commit_index));
  metrics_.set_gauge("nebulakv_raft_last_applied", static_cast<double>(node_status.last_applied));
  metrics_.set_gauge("nebulakv_raft_replication_lag",
                     static_cast<double>(node_status.last_log_index - node_status.commit_index));
  metrics_.set_gauge("nebulakv_cache_hit_ratio", storage_status.cache_hit_ratio);
  metrics_.set_gauge("nebulakv_cache_hits_total", static_cast<double>(storage_status.cache_hits));
  metrics_.set_gauge("nebulakv_cache_misses_total",
                     static_cast<double>(storage_status.cache_misses));
  metrics_.set_gauge("nebulakv_compactions_total",
                     static_cast<double>(storage_status.compactions_completed));
  metrics_.set_gauge("nebulakv_request_queue_depth",
                     static_cast<double>(executor_status.queued_tasks));
  metrics_.set_gauge("nebulakv_active_requests", static_cast<double>(executor_status.active_tasks));
}

} // namespace nebulakv::distributed
