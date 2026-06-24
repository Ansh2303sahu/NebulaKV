#pragma once

#include "nebulakv/v1/key_value_service.grpc.pb.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace nebulakv::network {

struct GrpcClientOptions {
  std::string address{"127.0.0.1:5001"};
  std::chrono::milliseconds timeout{2000};
  std::size_t max_message_bytes{8U * 1024U * 1024U};
};

class GrpcClient final {
 public:
  explicit GrpcClient(GrpcClientOptions options = {});

  [[nodiscard]] grpc::Status put(std::string key, std::string value,
                                 v1::PutResponse& response) const;
  [[nodiscard]] grpc::Status get(std::string key,
                                 v1::GetResponse& response) const;
  [[nodiscard]] grpc::Status remove(std::string key,
                                    v1::DeleteResponse& response) const;
  [[nodiscard]] grpc::Status batch_put(
      std::vector<std::pair<std::string, std::string>> entries,
      v1::BatchPutResponse& response) const;
  [[nodiscard]] grpc::Status status(v1::StatusResponse& response) const;

 private:
  void set_deadline(grpc::ClientContext& context) const;

  GrpcClientOptions options_;
  std::unique_ptr<v1::KeyValueService::Stub> stub_;
};

}  // namespace nebulakv::network
