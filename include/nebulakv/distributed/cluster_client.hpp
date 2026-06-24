#pragma once

#include "nebulakv/v1/key_value_service.grpc.pb.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

namespace nebulakv::distributed {

struct ClusterClientOptions {
  std::vector<std::string> seed_addresses{"127.0.0.1:5001"};
  std::chrono::milliseconds timeout{2000};
  std::size_t max_message_bytes{64U * 1024U * 1024U};
  std::size_t max_redirects{3U};
};

class ClusterClient final {
public:
  explicit ClusterClient(ClusterClientOptions options = {});

  [[nodiscard]] grpc::Status put(std::string key, std::string value, v1::PutResponse& response);
  [[nodiscard]] grpc::Status get(std::string key, v1::GetResponse& response);
  [[nodiscard]] grpc::Status remove(std::string key, v1::DeleteResponse& response);
  [[nodiscard]] grpc::Status batch_put(std::vector<std::pair<std::string, std::string>> entries,
                                       v1::BatchPutResponse& response);
  [[nodiscard]] grpc::Status status(v1::StatusResponse& response);
  [[nodiscard]] std::string current_address() const;

private:
  template <typename Response, typename Invocation>
  [[nodiscard]] grpc::Status invoke_with_redirect(Response& response, Invocation&& invocation) {
    const auto deadline = std::chrono::system_clock::now() + options_.timeout;
    grpc::Status last_status{grpc::StatusCode::UNAVAILABLE, "no cluster endpoint was attempted"};
    for (std::size_t attempt = 0U; attempt <= options_.max_redirects; ++attempt) {
      std::string address;
      {
        std::lock_guard lock{mutex_};
        address = current_address_;
      }
      auto stub = make_stub(address);
      grpc::ClientContext context;
      context.set_deadline(deadline);
      response.Clear();
      last_status = invocation(*stub, context, response);
      if (last_status.ok() || last_status.error_code() != grpc::StatusCode::FAILED_PRECONDITION) {
        return last_status;
      }
      const std::string leader = leader_from_metadata(context);
      if (leader.empty() || leader == address) {
        return last_status;
      }
      {
        std::lock_guard lock{mutex_};
        current_address_ = leader;
      }
    }
    return last_status;
  }
  [[nodiscard]] std::unique_ptr<v1::KeyValueService::Stub>
  make_stub(const std::string& address) const;
  [[nodiscard]] static std::string leader_from_metadata(const grpc::ClientContext& context);

  ClusterClientOptions options_;
  mutable std::mutex mutex_;
  std::string current_address_;
};

} // namespace nebulakv::distributed
