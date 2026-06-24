#include "nebulakv/durability_mode.hpp"
#include "nebulakv/network/grpc_server.hpp"
#include "nebulakv/persistent_key_value_store.hpp"

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include <pthread.h>

namespace {

struct Arguments {
  std::string host{"0.0.0.0"};
  std::uint16_t port{5001U};
  std::filesystem::path data_directory{"data/server"};
  std::size_t workers{8U};
  std::size_t queue_capacity{1024U};
  std::size_t max_message_bytes{8U * 1024U * 1024U};
  std::size_t max_batch_entries{1024U};
  std::size_t max_batch_bytes{8U * 1024U * 1024U};
  nebulakv::DurabilityMode durability{nebulakv::DurabilityMode::Sync};
  bool checkpoint_on_shutdown{false};
};

[[noreturn]] void usage(const char* program, const int exit_code) {
  std::ostream& output = exit_code == 0 ? std::cout : std::cerr;
  output << "Usage: " << program << " [options]\n"
         << "  --host <address>                 default: 0.0.0.0\n"
         << "  --port <port>                    default: 5001\n"
         << "  --data-dir <path>                default: data/server\n"
         << "  --workers <count>                default: 8\n"
         << "  --queue-capacity <count>         default: 1024\n"
         << "  --max-message-bytes <bytes>      default: 8388608\n"
         << "  --max-batch-entries <count>      default: 1024\n"
         << "  --max-batch-bytes <bytes>        default: 8388608\n"
         << "  --durability <sync|batch|none>   default: sync\n"
         << "  --checkpoint-on-shutdown\n";
  std::exit(exit_code);
}

template <typename Integer>
[[nodiscard]] Integer parse_integer(const std::string_view text,
                                    const std::string_view option) {
  Integer value{};
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto [position, error] = std::from_chars(begin, end, value);
  if (error != std::errc{} || position != end) {
    throw std::invalid_argument{std::string{option} + " requires an integer"};
  }
  return value;
}

[[nodiscard]] std::string_view require_value(const int argc, char** argv,
                                             int& index,
                                             const std::string_view option) {
  if (index + 1 >= argc) {
    throw std::invalid_argument{std::string{option} + " requires a value"};
  }
  ++index;
  return argv[index];
}

[[nodiscard]] nebulakv::DurabilityMode parse_durability(
    const std::string_view value) {
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

[[nodiscard]] Arguments parse_arguments(const int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view option{argv[index]};
    if (option == "--help" || option == "-h") {
      usage(argv[0], 0);
    }
    if (option == "--host") {
      arguments.host = require_value(argc, argv, index, option);
    } else if (option == "--port") {
      arguments.port = parse_integer<std::uint16_t>(
          require_value(argc, argv, index, option), option);
    } else if (option == "--data-dir") {
      arguments.data_directory = require_value(argc, argv, index, option);
    } else if (option == "--workers") {
      arguments.workers = parse_integer<std::size_t>(
          require_value(argc, argv, index, option), option);
    } else if (option == "--queue-capacity") {
      arguments.queue_capacity = parse_integer<std::size_t>(
          require_value(argc, argv, index, option), option);
    } else if (option == "--max-message-bytes") {
      arguments.max_message_bytes = parse_integer<std::size_t>(
          require_value(argc, argv, index, option), option);
    } else if (option == "--max-batch-entries") {
      arguments.max_batch_entries = parse_integer<std::size_t>(
          require_value(argc, argv, index, option), option);
    } else if (option == "--max-batch-bytes") {
      arguments.max_batch_bytes = parse_integer<std::size_t>(
          require_value(argc, argv, index, option), option);
    } else if (option == "--durability") {
      arguments.durability =
          parse_durability(require_value(argc, argv, index, option));
    } else if (option == "--checkpoint-on-shutdown") {
      arguments.checkpoint_on_shutdown = true;
    } else {
      throw std::invalid_argument{"unknown option: " + std::string{option}};
    }
  }
  return arguments;
}

}  // namespace

int main(const int argc, char** argv) {
  try {
    const Arguments arguments = parse_arguments(argc, argv);
    std::filesystem::create_directories(arguments.data_directory);

    sigset_t signal_set;
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signal_set, nullptr) != 0) {
      throw std::runtime_error{"failed to configure shutdown signal handling"};
    }

    nebulakv::PersistentStoreOptions store_options;
    store_options.wal_path = arguments.data_directory / "nebulakv.wal";
    store_options.sstable_directory = arguments.data_directory / "sstables";
    store_options.durability_mode = arguments.durability;
    nebulakv::PersistentKeyValueStore store{std::move(store_options)};

    nebulakv::network::GrpcServerOptions server_options;
    server_options.listen_address =
        arguments.host + ":" + std::to_string(arguments.port);
    server_options.worker_threads = arguments.workers;
    server_options.request_queue_capacity = arguments.queue_capacity;
    server_options.max_message_bytes = arguments.max_message_bytes;
    server_options.max_batch_entries = arguments.max_batch_entries;
    server_options.max_batch_bytes = arguments.max_batch_bytes;
    server_options.checkpoint_on_shutdown = arguments.checkpoint_on_shutdown;

    nebulakv::network::GrpcServer server{store, std::move(server_options)};
    server.start();

    std::cout << "NebulaKV gRPC server\n"
              << "listen_address=" << arguments.host << ':' << server.bound_port()
              << '\n'
              << "data_directory=" << arguments.data_directory << '\n'
              << "durability=" << nebulakv::to_string(store.durability_mode())
              << '\n'
              << "worker_threads=" << arguments.workers << '\n'
              << "queue_capacity=" << arguments.queue_capacity << '\n'
              << "max_message_bytes=" << arguments.max_message_bytes << '\n';

    std::atomic<bool> server_stopped{false};
    std::thread signal_thread([&server, &signal_set, &server_stopped] {
      while (!server_stopped.load(std::memory_order_acquire)) {
        timespec timeout{};
        timeout.tv_nsec = 200'000'000L;
        const int received_signal = sigtimedwait(&signal_set, nullptr, &timeout);
        if (received_signal == SIGINT || received_signal == SIGTERM) {
          std::cout << "shutdown_signal=" << received_signal << '\n';
          server.request_shutdown(std::chrono::seconds{10});
          return;
        }
        if (received_signal < 0 && errno != EAGAIN && errno != EINTR) {
          server.request_shutdown(std::chrono::seconds{10});
          return;
        }
      }
    });

    server.wait();
    server_stopped.store(true, std::memory_order_release);
    signal_thread.join();

    const auto service = server.service_statistics();
    const auto executor = server.executor_statistics();
    std::cout << "shutdown=complete\n"
              << "requests_total=" << service.requests_total << '\n'
              << "failed_requests_total=" << service.failed_requests_total
              << '\n'
              << "rejected_requests=" << executor.rejected_tasks << '\n';
  } catch (const std::exception& error) {
    std::cerr << "NebulaKV server failed: " << error.what() << '\n';
    return 1;
  }

  return 0;
}
