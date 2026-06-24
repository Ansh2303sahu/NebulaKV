#include "nebulakv/network/grpc_client.hpp"

#include <chrono>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nebulakv::network {

namespace {

[[nodiscard]] int checked_message_size(const std::size_t bytes) {
  if (bytes == 0U || bytes > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::invalid_argument{"maximum message size must fit in a positive signed integer"};
  }
  return static_cast<int>(bytes);
}

} // namespace

GrpcClient::GrpcClient(GrpcClientOptions options) : options_{std::move(options)} {
  if (options_.address.empty()) {
    throw std::invalid_argument{"server address must not be empty"};
  }
  if (options_.timeout <= std::chrono::milliseconds{0}) {
    throw std::invalid_argument{"request timeout must be positive"};
  }

  grpc::ChannelArguments arguments;
  const int max_message_size = checked_message_size(options_.max_message_bytes);
  arguments.SetMaxReceiveMessageSize(max_message_size);
  arguments.SetMaxSendMessageSize(max_message_size);
  auto channel =
      grpc::CreateCustomChannel(options_.address, grpc::InsecureChannelCredentials(), arguments);
  stub_ = v1::KeyValueService::NewStub(std::move(channel));
}

grpc::Status GrpcClient::put(std::string key, std::string value, v1::PutResponse& response) const {
  grpc::ClientContext context;
  set_deadline(context);
  v1::PutRequest request;
  request.set_key(std::move(key));
  request.set_value(std::move(value));
  return stub_->Put(&context, request, &response);
}

grpc::Status GrpcClient::get(std::string key, v1::GetResponse& response) const {
  grpc::ClientContext context;
  set_deadline(context);
  v1::GetRequest request;
  request.set_key(std::move(key));
  return stub_->Get(&context, request, &response);
}

grpc::Status GrpcClient::remove(std::string key, v1::DeleteResponse& response) const {
  grpc::ClientContext context;
  set_deadline(context);
  v1::DeleteRequest request;
  request.set_key(std::move(key));
  return stub_->Delete(&context, request, &response);
}

grpc::Status GrpcClient::batch_put(std::vector<std::pair<std::string, std::string>> entries,
                                   v1::BatchPutResponse& response) const {
  grpc::ClientContext context;
  set_deadline(context);
  v1::BatchPutRequest request;
  for (auto& [key, value] : entries) {
    auto* item = request.add_entries();
    item->set_key(std::move(key));
    item->set_value(std::move(value));
  }
  return stub_->BatchPut(&context, request, &response);
}

grpc::Status GrpcClient::status(v1::StatusResponse& response) const {
  grpc::ClientContext context;
  set_deadline(context);
  v1::StatusRequest request;
  return stub_->Status(&context, request, &response);
}

void GrpcClient::set_deadline(grpc::ClientContext& context) const {
  context.set_deadline(std::chrono::system_clock::now() + options_.timeout);
}

} // namespace nebulakv::network
