#pragma once

#include "nebulakv/network/bounded_executor.hpp"
#include "nebulakv/network/request_processor.hpp"
#include "nebulakv/v1/key_value_service.grpc.pb.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <grpcpp/grpcpp.h>

namespace nebulakv::network {

struct GrpcServiceOptions {
  std::size_t max_request_bytes{8U * 1024U * 1024U};
};

class KeyValueServiceImpl final : public v1::KeyValueService::Service {
 public:
  KeyValueServiceImpl(RequestProcessor& processor, BoundedExecutor& executor,
                      GrpcServiceOptions options = {});

  grpc::Status Put(grpc::ServerContext* context, const v1::PutRequest* request,
                   v1::PutResponse* response) override;
  grpc::Status Get(grpc::ServerContext* context, const v1::GetRequest* request,
                   v1::GetResponse* response) override;
  grpc::Status Delete(grpc::ServerContext* context,
                      const v1::DeleteRequest* request,
                      v1::DeleteResponse* response) override;
  grpc::Status BatchPut(grpc::ServerContext* context,
                        const v1::BatchPutRequest* request,
                        v1::BatchPutResponse* response) override;
  grpc::Status Status(grpc::ServerContext* context,
                      const v1::StatusRequest* request,
                      v1::StatusResponse* response) override;

  [[nodiscard]] ServiceStatistics statistics() const noexcept;

 private:
  [[nodiscard]] bool request_too_large(std::size_t encoded_size) const noexcept;
  [[nodiscard]] grpc::Status status_from_error(const ApiError& error) const;
  void record_failure() noexcept;

  RequestProcessor& processor_;
  BoundedExecutor& executor_;
  GrpcServiceOptions options_;
  std::atomic<std::uint64_t> requests_total_{0};
  std::atomic<std::uint64_t> failed_requests_total_{0};
};

}  // namespace nebulakv::network
