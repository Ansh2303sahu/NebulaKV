#pragma once

#include "nebulakv/network/bounded_executor.hpp"
#include "nebulakv/observability/json_logger.hpp"
#include "nebulakv/observability/metrics.hpp"
#include "nebulakv/raft/node.hpp"
#include "nebulakv/raft/state_machine.hpp"
#include "nebulakv/v1/key_value_service.grpc.pb.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include <grpcpp/grpcpp.h>

namespace nebulakv::distributed {

struct ClusterServiceOptions {
  std::string advertised_address;
  std::size_t max_request_bytes{8U * 1024U * 1024U};
  std::size_t max_batch_entries{1024U};
  std::size_t max_batch_bytes{8U * 1024U * 1024U};
};

struct ClusterServiceStatistics {
  std::uint64_t requests_total{0};
  std::uint64_t failed_requests_total{0};
  std::uint64_t redirects_total{0};
};

class ClusterKeyValueServiceImpl final : public v1::KeyValueService::Service {
public:
  ClusterKeyValueServiceImpl(raft::RaftNode& node, raft::StateMachine& state_machine,
                             network::BoundedExecutor& executor,
                             observability::MetricsRegistry& metrics,
                             observability::JsonLogger& logger, ClusterServiceOptions options);

  grpc::Status Put(grpc::ServerContext* context, const v1::PutRequest* request,
                   v1::PutResponse* response) override;
  grpc::Status Get(grpc::ServerContext* context, const v1::GetRequest* request,
                   v1::GetResponse* response) override;
  grpc::Status Delete(grpc::ServerContext* context, const v1::DeleteRequest* request,
                      v1::DeleteResponse* response) override;
  grpc::Status BatchPut(grpc::ServerContext* context, const v1::BatchPutRequest* request,
                        v1::BatchPutResponse* response) override;
  grpc::Status Status(grpc::ServerContext* context, const v1::StatusRequest* request,
                      v1::StatusResponse* response) override;

  [[nodiscard]] ClusterServiceStatistics statistics() const noexcept;

private:
  [[nodiscard]] grpc::Status not_leader(grpc::ServerContext& context, v1::ErrorDetail* error);
  [[nodiscard]] grpc::Status submit_status(grpc::ServerContext& context,
                                           const raft::SubmitResult& result,
                                           v1::ErrorDetail* error);
  [[nodiscard]] std::chrono::steady_clock::time_point
  steady_deadline(const grpc::ServerContext& context) const;
  void record_request(std::string_view operation);
  void record_failure(std::string_view operation);
  void record_latency(std::string_view operation, std::chrono::steady_clock::time_point started);
  void refresh_raft_metrics();

  raft::RaftNode& node_;
  raft::StateMachine& state_machine_;
  network::BoundedExecutor& executor_;
  observability::MetricsRegistry& metrics_;
  observability::JsonLogger& logger_;
  ClusterServiceOptions options_;
  std::atomic<std::uint64_t> requests_total_{0};
  std::atomic<std::uint64_t> failed_requests_total_{0};
  std::atomic<std::uint64_t> redirects_total_{0};
};

} // namespace nebulakv::distributed
