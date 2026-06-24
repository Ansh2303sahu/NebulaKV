#include "nebulakv/distributed/cluster_client.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nebulakv::distributed {

namespace {

[[nodiscard]] int checked_message_size(const std::size_t bytes) {
  if (bytes == 0U || bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument{"maximum client message size is invalid"};
  }
  return static_cast<int>(bytes);
}

} // namespace

ClusterClient::ClusterClient(ClusterClientOptions options) : options_{std::move(options)} {
  if (options_.seed_addresses.empty()) {
    throw std::invalid_argument{"at least one cluster seed address is required"};
  }
  for (const auto& address : options_.seed_addresses) {
    if (address.empty()) {
      throw std::invalid_argument{"cluster seed address must not be empty"};
    }
  }
  if (options_.timeout <= std::chrono::milliseconds{0}) {
    throw std::invalid_argument{"cluster client timeout must be positive"};
  }
  static_cast<void>(checked_message_size(options_.max_message_bytes));
  current_address_ = options_.seed_addresses.front();
}

grpc::Status ClusterClient::put(std::string key, std::string value, v1::PutResponse& response) {
  v1::PutRequest request;
  request.set_key(std::move(key));
  request.set_value(std::move(value));
  return invoke_with_redirect(
      response,
      [&request](v1::KeyValueService::Stub& stub, grpc::ClientContext& context,
                 v1::PutResponse& target) { return stub.Put(&context, request, &target); });
}

grpc::Status ClusterClient::get(std::string key, v1::GetResponse& response) {
  v1::GetRequest request;
  request.set_key(std::move(key));
  return invoke_with_redirect(
      response,
      [&request](v1::KeyValueService::Stub& stub, grpc::ClientContext& context,
                 v1::GetResponse& target) { return stub.Get(&context, request, &target); });
}

grpc::Status ClusterClient::remove(std::string key, v1::DeleteResponse& response) {
  v1::DeleteRequest request;
  request.set_key(std::move(key));
  return invoke_with_redirect(
      response,
      [&request](v1::KeyValueService::Stub& stub, grpc::ClientContext& context,
                 v1::DeleteResponse& target) { return stub.Delete(&context, request, &target); });
}

grpc::Status ClusterClient::batch_put(std::vector<std::pair<std::string, std::string>> entries,
                                      v1::BatchPutResponse& response) {
  v1::BatchPutRequest request;
  for (auto& [key, value] : entries) {
    auto* entry = request.add_entries();
    entry->set_key(std::move(key));
    entry->set_value(std::move(value));
  }
  return invoke_with_redirect(response, [&request](v1::KeyValueService::Stub& stub,
                                                   grpc::ClientContext& context,
                                                   v1::BatchPutResponse& target) {
    return stub.BatchPut(&context, request, &target);
  });
}

grpc::Status ClusterClient::status(v1::StatusResponse& response) {
  v1::StatusRequest request;
  return invoke_with_redirect(
      response,
      [&request](v1::KeyValueService::Stub& stub, grpc::ClientContext& context,
                 v1::StatusResponse& target) { return stub.Status(&context, request, &target); });
}

std::string ClusterClient::current_address() const {
  std::lock_guard lock{mutex_};
  return current_address_;
}

std::unique_ptr<v1::KeyValueService::Stub>
ClusterClient::make_stub(const std::string& address) const {
  grpc::ChannelArguments arguments;
  const int maximum = checked_message_size(options_.max_message_bytes);
  arguments.SetMaxReceiveMessageSize(maximum);
  arguments.SetMaxSendMessageSize(maximum);
  auto channel = grpc::CreateCustomChannel(address, grpc::InsecureChannelCredentials(), arguments);
  return v1::KeyValueService::NewStub(std::move(channel));
}

std::string ClusterClient::leader_from_metadata(const grpc::ClientContext& context) {
  const auto& metadata = context.GetServerTrailingMetadata();
  const auto iterator = metadata.find("nebulakv-leader-address");
  if (iterator == metadata.end()) {
    return {};
  }
  return {iterator->second.data(), iterator->second.length()};
}

} // namespace nebulakv::distributed
