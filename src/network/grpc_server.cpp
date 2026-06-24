#include "nebulakv/network/grpc_server.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>

namespace nebulakv::network {

namespace {

[[nodiscard]] int checked_message_size(const std::size_t bytes) {
  if (bytes == 0U || bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument{"maximum message size must fit in a positive signed integer"};
  }
  return static_cast<int>(bytes);
}

} // namespace

GrpcServer::GrpcServer(PersistentKeyValueStore& store, GrpcServerOptions options)
    : store_{store}, options_{std::move(options)},
      executor_{options_.worker_threads, options_.request_queue_capacity},
      processor_{store_,
                 RequestProcessorOptions{options_.max_batch_entries, options_.max_batch_bytes}},
      service_{processor_, executor_, GrpcServiceOptions{options_.max_message_bytes}} {
  if (options_.listen_address.empty()) {
    throw std::invalid_argument{"listen address must not be empty"};
  }
  static_cast<void>(checked_message_size(options_.max_message_bytes));
}

GrpcServer::~GrpcServer() {
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

void GrpcServer::start() {
  std::lock_guard lock{lifecycle_mutex_};
  if (started_) {
    throw std::logic_error{"gRPC server has already been started"};
  }

  grpc::ServerBuilder builder;
  const int max_message_size = checked_message_size(options_.max_message_bytes);
  builder.SetMaxReceiveMessageSize(max_message_size);
  builder.SetMaxSendMessageSize(max_message_size);
  builder.AddListeningPort(options_.listen_address, grpc::InsecureServerCredentials(),
                           &bound_port_);
  builder.RegisterService(&service_);
  server_ = builder.BuildAndStart();
  if (!server_ || bound_port_ == 0) {
    throw std::runtime_error{"failed to bind the gRPC server"};
  }
  started_ = true;
}

void GrpcServer::wait() {
  {
    std::lock_guard lock{lifecycle_mutex_};
    if (!started_ || !server_) {
      throw std::logic_error{"gRPC server is not running"};
    }
  }

  server_->Wait();
  finish_shutdown();
}

void GrpcServer::request_shutdown(const std::chrono::milliseconds grace_period) {
  std::lock_guard lock{lifecycle_mutex_};
  if (!server_ || shutdown_requested_) {
    return;
  }

  shutdown_requested_ = true;
  server_->Shutdown(std::chrono::system_clock::now() + grace_period);
}

int GrpcServer::bound_port() const noexcept { return bound_port_; }

const std::string& GrpcServer::listen_address() const noexcept { return options_.listen_address; }

ServiceStatistics GrpcServer::service_statistics() const noexcept { return service_.statistics(); }

ExecutorStatistics GrpcServer::executor_statistics() const { return executor_.statistics(); }

void GrpcServer::finish_shutdown() {
  {
    std::lock_guard lock{lifecycle_mutex_};
    if (shutdown_finished_) {
      return;
    }
    shutdown_finished_ = true;
  }

  executor_.shutdown(true);
  if (options_.checkpoint_on_shutdown) {
    store_.checkpoint();
  } else {
    store_.flush();
  }
}

} // namespace nebulakv::network
