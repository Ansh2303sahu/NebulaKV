#include "nebulakv/durability_mode.hpp"
#include "nebulakv/network/grpc_client.hpp"
#include "nebulakv/network/grpc_server.hpp"
#include "nebulakv/persistent_key_value_store.hpp"
#include "nebulakv/v1/key_value_service.grpc.pb.h"
#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

namespace {

using nebulakv::DurabilityMode;
using nebulakv::PersistentKeyValueStore;
using nebulakv::PersistentStoreOptions;
using nebulakv::network::GrpcClient;
using nebulakv::network::GrpcClientOptions;
using nebulakv::network::GrpcServer;
using nebulakv::network::GrpcServerOptions;
using nebulakv::test::TemporaryDirectory;

[[nodiscard]] PersistentStoreOptions store_options(
    const TemporaryDirectory& directory,
    const DurabilityMode durability = DurabilityMode::Sync) {
  PersistentStoreOptions options;
  options.wal_path = directory.file("nebulakv.wal");
  options.sstable_directory = directory.file("sstables");
  options.durability_mode = durability;
  options.enable_automatic_compaction = false;
  return options;
}

[[nodiscard]] GrpcClient make_client(const GrpcServer& server) {
  GrpcClientOptions options;
  options.address = "127.0.0.1:" + std::to_string(server.bound_port());
  options.timeout = std::chrono::seconds{5};
  return GrpcClient{std::move(options)};
}

TEST(GrpcServiceTest, SupportsRemotePutGetDeleteBatchAndStatus) {
  TemporaryDirectory directory;
  PersistentKeyValueStore store{store_options(directory)};
  GrpcServerOptions server_options;
  server_options.listen_address = "127.0.0.1:0";
  GrpcServer server{store, server_options};
  server.start();
  GrpcClient client = make_client(server);

  nebulakv::v1::PutResponse put_response;
  ASSERT_TRUE(client.put("user:1", "Ansh", put_response).ok());
  EXPECT_TRUE(put_response.success());

  nebulakv::v1::GetResponse get_response;
  ASSERT_TRUE(client.get("user:1", get_response).ok());
  EXPECT_TRUE(get_response.found());
  EXPECT_EQ(get_response.value(), "Ansh");

  std::vector<std::pair<std::string, std::string>> batch{{"a", "1"},
                                                         {"b", "2"}};
  nebulakv::v1::BatchPutResponse batch_response;
  ASSERT_TRUE(client.batch_put(std::move(batch), batch_response).ok());
  EXPECT_EQ(batch_response.writes_applied(), 2U);

  nebulakv::v1::StatusResponse status_response;
  ASSERT_TRUE(client.status(status_response).ok());
  EXPECT_TRUE(status_response.ready());
  EXPECT_EQ(status_response.live_keys(), 3U);

  nebulakv::v1::DeleteResponse delete_response;
  ASSERT_TRUE(client.remove("user:1", delete_response).ok());
  EXPECT_TRUE(delete_response.deleted());

  server.request_shutdown();
  server.wait();
}

TEST(GrpcServiceTest, ReturnsInvalidArgumentForInvalidRequest) {
  TemporaryDirectory directory;
  PersistentKeyValueStore store{store_options(directory)};
  GrpcServerOptions server_options;
  server_options.listen_address = "127.0.0.1:0";
  GrpcServer server{store, server_options};
  server.start();
  GrpcClient client = make_client(server);

  nebulakv::v1::PutResponse response;
  const grpc::Status status = client.put("", "value", response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  server.request_shutdown();
  server.wait();
}

TEST(GrpcServiceTest, HonorsExpiredClientDeadline) {
  TemporaryDirectory directory;
  PersistentKeyValueStore store{store_options(directory)};
  GrpcServerOptions server_options;
  server_options.listen_address = "127.0.0.1:0";
  GrpcServer server{store, server_options};
  server.start();

  const std::string address = "127.0.0.1:" + std::to_string(server.bound_port());
  auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
  auto stub = nebulakv::v1::KeyValueService::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() -
                       std::chrono::milliseconds{1});
  nebulakv::v1::GetRequest request;
  request.set_key("key");
  nebulakv::v1::GetResponse response;
  const grpc::Status status = stub->Get(&context, request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);

  server.request_shutdown();
  server.wait();
}

TEST(GrpcServiceTest, SupportsConcurrentClients) {
  TemporaryDirectory directory;
  PersistentKeyValueStore store{store_options(directory)};
  GrpcServerOptions server_options;
  server_options.listen_address = "127.0.0.1:0";
  server_options.worker_threads = 4U;
  server_options.request_queue_capacity = 128U;
  GrpcServer server{store, server_options};
  server.start();

  constexpr std::size_t kClients = 8U;
  constexpr std::size_t kWritesPerClient = 25U;
  std::atomic<bool> all_requests_succeeded{true};
  std::vector<std::thread> clients;
  clients.reserve(kClients);
  for (std::size_t client_index = 0; client_index < kClients; ++client_index) {
    clients.emplace_back([&server, &all_requests_succeeded, client_index] {
      GrpcClient client = make_client(server);
      for (std::size_t write_index = 0; write_index < kWritesPerClient;
           ++write_index) {
        nebulakv::v1::PutResponse response;
        const std::string key = "client:" + std::to_string(client_index) + ':' +
                                std::to_string(write_index);
        if (!client.put(key, "value", response).ok()) {
          all_requests_succeeded.store(false, std::memory_order_relaxed);
          return;
        }
      }
    });
  }
  for (auto& client : clients) {
    client.join();
  }

  EXPECT_TRUE(all_requests_succeeded.load(std::memory_order_relaxed));
  EXPECT_EQ(store.size(), kClients * kWritesPerClient);
  server.request_shutdown();
  server.wait();
}

TEST(GrpcServiceTest, GracefulShutdownFlushesAcknowledgedBatchWrites) {
  TemporaryDirectory directory;
  {
    PersistentKeyValueStore store{
        store_options(directory, DurabilityMode::Batch)};
    GrpcServerOptions server_options;
    server_options.listen_address = "127.0.0.1:0";
    GrpcServer server{store, server_options};
    server.start();
    GrpcClient client = make_client(server);

    nebulakv::v1::PutResponse response;
    ASSERT_TRUE(client.put("durable", "value", response).ok());
    ASSERT_TRUE(response.success());

    server.request_shutdown();
    server.wait();
  }

  PersistentKeyValueStore reopened{store_options(directory)};
  EXPECT_EQ(reopened.get("durable"), "value");
}

}  // namespace
