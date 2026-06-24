#include "nebulakv/distributed/cluster_server.hpp"
#include "nebulakv/distributed/grpc_raft_transport.hpp"
#include "nebulakv/durability_mode.hpp"
#include "nebulakv/observability/json_logger.hpp"
#include "nebulakv/raft/fault_injecting_transport.hpp"
#include "nebulakv/raft/node.hpp"
#include "nebulakv/raft/state_machine.hpp"
#include "nebulakv/raft/storage.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <pthread.h>

namespace {

struct Arguments {
  std::string node_id;
  std::string listen_address{"0.0.0.0:5001"};
  std::string advertised_address{"127.0.0.1:5001"};
  std::vector<nebulakv::raft::Peer> peers;
  std::filesystem::path data_directory{"data/node-1"};
  std::size_t workers{8U};
  std::size_t queue_capacity{1024U};
  std::size_t max_message_bytes{64U * 1024U * 1024U};
  std::uint16_t metrics_port{9100U};
  std::string metrics_host{"0.0.0.0"};
  std::chrono::milliseconds election_min{300};
  std::chrono::milliseconds election_max{600};
  std::chrono::milliseconds heartbeat{75};
  std::chrono::milliseconds rpc_timeout{200};
  std::uint64_t snapshot_threshold{10'000U};
  nebulakv::DurabilityMode durability{nebulakv::DurabilityMode::Sync};
  double drop_probability{0.0};
  std::chrono::milliseconds fault_delay{0};
};

[[noreturn]] void usage(const char* program, const int exit_code) {
  std::ostream& output = exit_code == 0 ? std::cout : std::cerr;
  output << "Usage: " << program << " --node-id <id> [options]\n"
         << "  --listen <host:port>              default: 0.0.0.0:5001\n"
         << "  --advertise <host:port>           default: 127.0.0.1:5001\n"
         << "  --peer <id=host:port>             repeat for every other node\n"
         << "  --data-dir <path>                 default: data/node-1\n"
         << "  --workers <count>                 default: 8\n"
         << "  --queue-capacity <count>          default: 1024\n"
         << "  --metrics-host <address>          default: 0.0.0.0\n"
         << "  --metrics-port <port>             default: 9100\n"
         << "  --election-min-ms <milliseconds>  default: 300\n"
         << "  --election-max-ms <milliseconds>  default: 600\n"
         << "  --heartbeat-ms <milliseconds>     default: 75\n"
         << "  --rpc-timeout-ms <milliseconds>   default: 200\n"
         << "  --snapshot-threshold <entries>    default: 10000\n"
         << "  --durability <sync|batch|none>    default: sync\n"
         << "  --fault-drop-probability <0..1>   default: 0\n"
         << "  --fault-delay-ms <milliseconds>   default: 0\n";
  std::exit(exit_code);
}

template <typename Integer>
[[nodiscard]] Integer parse_integer(const std::string_view text, const std::string_view option) {
  Integer value{};
  const auto [position, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || position != text.data() + text.size()) {
    throw std::invalid_argument{std::string{option} + " requires an integer"};
  }
  return value;
}

[[nodiscard]] double parse_double(const std::string_view text, const std::string_view option) {
  std::string owned{text};
  char* end = nullptr;
  errno = 0;
  const double value = std::strtod(owned.c_str(), &end);
  if (errno != 0 || end != owned.c_str() + owned.size()) {
    throw std::invalid_argument{std::string{option} + " requires a number"};
  }
  return value;
}

[[nodiscard]] std::string_view require_value(const int argc, char** argv, int& index,
                                             const std::string_view option) {
  if (index + 1 >= argc) {
    throw std::invalid_argument{std::string{option} + " requires a value"};
  }
  ++index;
  return argv[index];
}

[[nodiscard]] nebulakv::DurabilityMode parse_durability(const std::string_view value) {
  if (value == "sync") {
    return nebulakv::DurabilityMode::Sync;
  }
  if (value == "batch") {
    return nebulakv::DurabilityMode::Batch;
  }
  if (value == "none") {
    return nebulakv::DurabilityMode::None;
  }
  throw std::invalid_argument{"durability must be sync, batch, or none"};
}

[[nodiscard]] nebulakv::raft::Peer parse_peer(const std::string_view value) {
  const auto separator = value.find('=');
  if (separator == std::string_view::npos || separator == 0U || separator + 1U >= value.size()) {
    throw std::invalid_argument{"peer must use id=host:port format"};
  }
  return {std::string{value.substr(0U, separator)}, std::string{value.substr(separator + 1U)}};
}

[[nodiscard]] Arguments parse_arguments(const int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option{argv[index]};
    if (option == "--help" || option == "-h") {
      usage(argv[0], 0);
    }
    if (option == "--node-id") {
      arguments.node_id = require_value(argc, argv, index, option);
    } else if (option == "--listen") {
      arguments.listen_address = require_value(argc, argv, index, option);
    } else if (option == "--advertise") {
      arguments.advertised_address = require_value(argc, argv, index, option);
    } else if (option == "--peer") {
      arguments.peers.push_back(parse_peer(require_value(argc, argv, index, option)));
    } else if (option == "--data-dir") {
      arguments.data_directory = require_value(argc, argv, index, option);
    } else if (option == "--workers") {
      arguments.workers =
          parse_integer<std::size_t>(require_value(argc, argv, index, option), option);
    } else if (option == "--queue-capacity") {
      arguments.queue_capacity =
          parse_integer<std::size_t>(require_value(argc, argv, index, option), option);
    } else if (option == "--max-message-bytes") {
      arguments.max_message_bytes =
          parse_integer<std::size_t>(require_value(argc, argv, index, option), option);
    } else if (option == "--metrics-host") {
      arguments.metrics_host = require_value(argc, argv, index, option);
    } else if (option == "--metrics-port") {
      arguments.metrics_port =
          parse_integer<std::uint16_t>(require_value(argc, argv, index, option), option);
    } else if (option == "--election-min-ms") {
      arguments.election_min = std::chrono::milliseconds{
          parse_integer<std::int64_t>(require_value(argc, argv, index, option), option)};
    } else if (option == "--election-max-ms") {
      arguments.election_max = std::chrono::milliseconds{
          parse_integer<std::int64_t>(require_value(argc, argv, index, option), option)};
    } else if (option == "--heartbeat-ms") {
      arguments.heartbeat = std::chrono::milliseconds{
          parse_integer<std::int64_t>(require_value(argc, argv, index, option), option)};
    } else if (option == "--rpc-timeout-ms") {
      arguments.rpc_timeout = std::chrono::milliseconds{
          parse_integer<std::int64_t>(require_value(argc, argv, index, option), option)};
    } else if (option == "--snapshot-threshold") {
      arguments.snapshot_threshold =
          parse_integer<std::uint64_t>(require_value(argc, argv, index, option), option);
    } else if (option == "--durability") {
      arguments.durability = parse_durability(require_value(argc, argv, index, option));
    } else if (option == "--fault-drop-probability") {
      arguments.drop_probability = parse_double(require_value(argc, argv, index, option), option);
    } else if (option == "--fault-delay-ms") {
      arguments.fault_delay = std::chrono::milliseconds{
          parse_integer<std::int64_t>(require_value(argc, argv, index, option), option)};
    } else {
      throw std::invalid_argument{"unknown option: " + std::string{option}};
    }
  }
  if (arguments.node_id.empty()) {
    throw std::invalid_argument{"--node-id is required"};
  }
  return arguments;
}

} // namespace

int main(const int argc, char** argv) {
  try {
    const Arguments arguments = parse_arguments(argc, argv);
    std::filesystem::create_directories(arguments.data_directory);

    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signal_set, nullptr) != 0) {
      throw std::runtime_error{"failed to configure shutdown signals"};
    }

    nebulakv::observability::JsonLogger logger{std::cout};
    nebulakv::raft::RaftStorage raft_storage{arguments.data_directory / "raft"};
    nebulakv::raft::DurableStateMachineOptions state_options;
    state_options.directory = arguments.data_directory;
    state_options.durability_mode = arguments.durability;
    nebulakv::raft::DurableKeyValueStateMachine state_machine{std::move(state_options)};

    nebulakv::distributed::GrpcRaftTransport grpc_transport{arguments.peers,
                                                            arguments.max_message_bytes};
    nebulakv::raft::FaultInjectionOptions fault_options;
    fault_options.drop_probability = arguments.drop_probability;
    fault_options.fixed_delay = arguments.fault_delay;
    fault_options.random_seed =
        static_cast<std::uint64_t>(std::hash<std::string>{}(arguments.node_id));
    nebulakv::raft::FaultInjectingTransport transport{grpc_transport, fault_options};

    nebulakv::raft::RaftNodeOptions raft_options;
    raft_options.node_id = arguments.node_id;
    raft_options.peers = arguments.peers;
    raft_options.election_timeout_min = arguments.election_min;
    raft_options.election_timeout_max = arguments.election_max;
    raft_options.heartbeat_interval = arguments.heartbeat;
    raft_options.rpc_timeout = arguments.rpc_timeout;
    raft_options.snapshot_threshold_entries = arguments.snapshot_threshold;
    nebulakv::raft::RaftNode node{std::move(raft_options), raft_storage, state_machine, transport};

    nebulakv::distributed::ClusterServerOptions server_options;
    server_options.listen_address = arguments.listen_address;
    server_options.advertised_address = arguments.advertised_address;
    server_options.worker_threads = arguments.workers;
    server_options.request_queue_capacity = arguments.queue_capacity;
    server_options.max_message_bytes = arguments.max_message_bytes;
    server_options.metrics_host = arguments.metrics_host;
    server_options.metrics_port = arguments.metrics_port;
    nebulakv::distributed::ClusterServer server{node, state_machine, logger,
                                                std::move(server_options)};
    server.start();

    logger.log("info", "node_ready",
               {{"node_id", arguments.node_id},
                {"listen_address", arguments.listen_address},
                {"advertised_address", arguments.advertised_address},
                {"peer_count", std::to_string(arguments.peers.size())},
                {"data_directory", arguments.data_directory.string()}});

    std::atomic<bool> stopped{false};
    std::thread signal_thread([&server, &signal_set, &stopped] {
      while (!stopped.load(std::memory_order_acquire)) {
        timespec timeout{};
        timeout.tv_nsec = 200'000'000L;
        const int signal = sigtimedwait(&signal_set, nullptr, &timeout);
        if (signal == SIGINT || signal == SIGTERM) {
          server.request_shutdown(std::chrono::seconds{10});
          return;
        }
        if (signal < 0 && errno != EAGAIN && errno != EINTR) {
          server.request_shutdown(std::chrono::seconds{10});
          return;
        }
      }
    });

    server.wait();
    stopped.store(true, std::memory_order_release);
    signal_thread.join();
  } catch (const std::exception& error) {
    std::cerr << "NebulaKV node failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
