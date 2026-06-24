#pragma once

#include "nebulakv/network/bounded_executor.hpp"
#include "nebulakv/network/grpc_service.hpp"
#include "nebulakv/network/request_processor.hpp"
#include "nebulakv/persistent_key_value_store.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>

#include <grpcpp/grpcpp.h>

namespace nebulakv::network {

struct GrpcServerOptions {
  std::string listen_address{"0.0.0.0:5001"};
  std::size_t worker_threads{8U};
  std::size_t request_queue_capacity{1024U};
  std::size_t max_message_bytes{8U * 1024U * 1024U};
  std::size_t max_batch_entries{1024U};
  std::size_t max_batch_bytes{8U * 1024U * 1024U};
  bool checkpoint_on_shutdown{false};
};

class GrpcServer final {
 public:
  GrpcServer(PersistentKeyValueStore& store, GrpcServerOptions options = {});
  ~GrpcServer();

  GrpcServer(const GrpcServer&) = delete;
  GrpcServer& operator=(const GrpcServer&) = delete;
  GrpcServer(GrpcServer&&) = delete;
  GrpcServer& operator=(GrpcServer&&) = delete;

  void start();
  void wait();
  void request_shutdown(
      std::chrono::milliseconds grace_period = std::chrono::seconds{10});

  [[nodiscard]] int bound_port() const noexcept;
  [[nodiscard]] const std::string& listen_address() const noexcept;
  [[nodiscard]] ServiceStatistics service_statistics() const noexcept;
  [[nodiscard]] ExecutorStatistics executor_statistics() const;

 private:
  void finish_shutdown();

  PersistentKeyValueStore& store_;
  GrpcServerOptions options_;
  BoundedExecutor executor_;
  RequestProcessor processor_;
  KeyValueServiceImpl service_;
  std::unique_ptr<grpc::Server> server_;
  mutable std::mutex lifecycle_mutex_;
  int bound_port_{0};
  bool started_{false};
  bool shutdown_requested_{false};
  bool shutdown_finished_{false};
};

}  // namespace nebulakv::network
