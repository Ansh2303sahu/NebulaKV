#pragma once

#include "nebulakv/distributed/cluster_service.hpp"
#include "nebulakv/distributed/raft_grpc_service.hpp"
#include "nebulakv/network/bounded_executor.hpp"
#include "nebulakv/observability/json_logger.hpp"
#include "nebulakv/observability/metrics.hpp"
#include "nebulakv/observability/metrics_http_server.hpp"
#include "nebulakv/raft/node.hpp"
#include "nebulakv/raft/state_machine.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <grpcpp/grpcpp.h>

namespace nebulakv::distributed {

struct ClusterServerOptions {
  std::string listen_address{"0.0.0.0:5001"};
  std::string advertised_address{"127.0.0.1:5001"};
  std::size_t worker_threads{8U};
  std::size_t request_queue_capacity{1024U};
  std::size_t max_message_bytes{64U * 1024U * 1024U};
  std::size_t max_batch_entries{1024U};
  std::size_t max_batch_bytes{8U * 1024U * 1024U};
  std::string metrics_host{"0.0.0.0"};
  std::uint16_t metrics_port{9100U};
};

class ClusterServer final {
public:
  ClusterServer(raft::RaftNode& node, raft::StateMachine& state_machine,
                observability::JsonLogger& logger, ClusterServerOptions options = {});
  ~ClusterServer();

  ClusterServer(const ClusterServer&) = delete;
  ClusterServer& operator=(const ClusterServer&) = delete;

  void start();
  void wait();
  void request_shutdown(std::chrono::milliseconds grace_period = std::chrono::seconds{10});

  [[nodiscard]] int bound_port() const noexcept;
  [[nodiscard]] std::uint16_t metrics_port() const noexcept;
  [[nodiscard]] ClusterServiceStatistics service_statistics() const noexcept;
  [[nodiscard]] network::ExecutorStatistics executor_statistics() const;
  [[nodiscard]] std::string metrics_text();

private:
  void finish_shutdown();
  void refresh_metrics();

  raft::RaftNode& node_;
  raft::StateMachine& state_machine_;
  observability::JsonLogger& logger_;
  ClusterServerOptions options_;
  network::BoundedExecutor executor_;
  observability::MetricsRegistry metrics_;
  ClusterKeyValueServiceImpl key_value_service_;
  RaftServiceImpl raft_service_;
  observability::MetricsHttpServer metrics_server_;
  std::unique_ptr<grpc::Server> server_;
  mutable std::mutex lifecycle_mutex_;
  int bound_port_{0};
  bool started_{false};
  bool shutdown_requested_{false};
  bool shutdown_finished_{false};
};

} // namespace nebulakv::distributed
