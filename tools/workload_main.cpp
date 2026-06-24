#include "nebulakv/distributed/cluster_client.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct Arguments {
  std::string host{"127.0.0.1"};
  std::uint16_t port{5001U};
  std::chrono::seconds duration{10};
  std::size_t clients{8U};
  std::size_t keyspace{10'000U};
  std::size_t value_bytes{256U};
  double read_ratio{0.8};
  std::chrono::milliseconds timeout{2000};
};

struct WorkerResult {
  std::uint64_t operations{0};
  std::uint64_t errors{0};
  std::uint64_t reads{0};
  std::uint64_t writes{0};
  std::vector<double> latency_microseconds;
};

[[noreturn]] void usage(const char* program, const int exit_code) {
  std::ostream& output = exit_code == 0 ? std::cout : std::cerr;
  output << "Usage: " << program << " [options]\n"
         << "  --host <address>            default: 127.0.0.1\n"
         << "  --port <port>               default: 5001\n"
         << "  --duration-seconds <count>  default: 10\n"
         << "  --clients <count>           default: 8\n"
         << "  --keyspace <count>          default: 10000\n"
         << "  --value-bytes <count>       default: 256\n"
         << "  --read-ratio <0..1>         default: 0.8\n"
         << "  --timeout-ms <count>        default: 2000\n";
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
  const std::string owned{text};
  char* end = nullptr;
  const double value = std::strtod(owned.c_str(), &end);
  if (end != owned.c_str() + owned.size()) {
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
      arguments.port =
          parse_integer<std::uint16_t>(require_value(argc, argv, index, option), option);
    } else if (option == "--duration-seconds") {
      arguments.duration = std::chrono::seconds{
          parse_integer<std::int64_t>(require_value(argc, argv, index, option), option)};
    } else if (option == "--clients") {
      arguments.clients =
          parse_integer<std::size_t>(require_value(argc, argv, index, option), option);
    } else if (option == "--keyspace") {
      arguments.keyspace =
          parse_integer<std::size_t>(require_value(argc, argv, index, option), option);
    } else if (option == "--value-bytes") {
      arguments.value_bytes =
          parse_integer<std::size_t>(require_value(argc, argv, index, option), option);
    } else if (option == "--read-ratio") {
      arguments.read_ratio = parse_double(require_value(argc, argv, index, option), option);
    } else if (option == "--timeout-ms") {
      arguments.timeout = std::chrono::milliseconds{
          parse_integer<std::int64_t>(require_value(argc, argv, index, option), option)};
    } else {
      throw std::invalid_argument{"unknown option: " + std::string{option}};
    }
  }
  if (arguments.duration <= std::chrono::seconds{0} || arguments.clients == 0U ||
      arguments.keyspace == 0U || arguments.value_bytes == 0U || arguments.read_ratio < 0.0 ||
      arguments.read_ratio > 1.0 || arguments.timeout <= std::chrono::milliseconds{0}) {
    throw std::invalid_argument{"workload options are outside valid ranges"};
  }
  return arguments;
}

[[nodiscard]] double percentile(const std::vector<double>& sorted, const double quantile) {
  if (sorted.empty()) {
    return 0.0;
  }
  const double position = quantile * static_cast<double>(sorted.size() - 1U);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  if (lower == upper) {
    return sorted[lower];
  }
  const double fraction = position - static_cast<double>(lower);
  return sorted[lower] + ((sorted[upper] - sorted[lower]) * fraction);
}

} // namespace

int main(const int argc, char** argv) {
  try {
    const Arguments arguments = parse_arguments(argc, argv);
    std::vector<WorkerResult> results(arguments.clients);
    std::vector<std::thread> workers;
    workers.reserve(arguments.clients);
    std::atomic<bool> start{false};
    const auto stop_at = std::chrono::steady_clock::now() + arguments.duration;

    for (std::size_t worker_index = 0U; worker_index < arguments.clients; ++worker_index) {
      workers.emplace_back([&, worker_index] {
        nebulakv::distributed::ClusterClientOptions client_options;
        client_options.seed_addresses = {arguments.host + ":" + std::to_string(arguments.port)};
        client_options.timeout = arguments.timeout;
        nebulakv::distributed::ClusterClient client{std::move(client_options)};
        std::mt19937_64 random{static_cast<std::uint64_t>(worker_index + 1U)};
        std::uniform_int_distribution<std::size_t> key_distribution{0U, arguments.keyspace - 1U};
        std::bernoulli_distribution read_distribution{arguments.read_ratio};
        WorkerResult& result = results[worker_index];
        result.latency_microseconds.reserve(100'000U);
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        while (std::chrono::steady_clock::now() < stop_at) {
          const std::size_t key_number = key_distribution(random);
          const std::string key = "key-" + std::to_string(key_number);
          const auto operation_started = std::chrono::steady_clock::now();
          grpc::Status status;
          if (read_distribution(random)) {
            nebulakv::v1::GetResponse response;
            status = client.get(key, response);
            ++result.reads;
          } else {
            nebulakv::v1::PutResponse response;
            std::string value(arguments.value_bytes, 'x');
            if (!value.empty()) {
              value.front() = static_cast<char>('a' + (key_number % 26U));
            }
            status = client.put(key, std::move(value), response);
            ++result.writes;
          }
          const double latency = std::chrono::duration<double, std::micro>(
                                     std::chrono::steady_clock::now() - operation_started)
                                     .count();
          if (result.latency_microseconds.size() < 1'000'000U) {
            result.latency_microseconds.push_back(latency);
          }
          ++result.operations;
          if (!status.ok()) {
            ++result.errors;
          }
        }
      });
    }

    const auto actual_start = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
      worker.join();
    }
    const double elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - actual_start).count();

    std::uint64_t total_operations = 0U;
    std::uint64_t total_errors = 0U;
    std::uint64_t total_reads = 0U;
    std::uint64_t total_writes = 0U;
    std::vector<double> latencies;
    for (auto& result : results) {
      total_operations += result.operations;
      total_errors += result.errors;
      total_reads += result.reads;
      total_writes += result.writes;
      latencies.insert(latencies.end(), result.latency_microseconds.begin(),
                       result.latency_microseconds.end());
    }
    std::sort(latencies.begin(), latencies.end());
    const double throughput =
        elapsed_seconds == 0.0 ? 0.0 : static_cast<double>(total_operations) / elapsed_seconds;
    const double error_rate = total_operations == 0U ? 0.0
                                                     : static_cast<double>(total_errors) /
                                                           static_cast<double>(total_operations);

    std::cout << "clients=" << arguments.clients << '\n'
              << "duration_seconds=" << elapsed_seconds << '\n'
              << "operations=" << total_operations << '\n'
              << "reads=" << total_reads << '\n'
              << "writes=" << total_writes << '\n'
              << "operations_per_second=" << throughput << '\n'
              << "error_rate=" << error_rate << '\n'
              << "p50_latency_us=" << percentile(latencies, 0.50) << '\n'
              << "p95_latency_us=" << percentile(latencies, 0.95) << '\n'
              << "p99_latency_us=" << percentile(latencies, 0.99) << '\n'
              << "max_latency_us=" << (latencies.empty() ? 0.0 : latencies.back()) << '\n';
  } catch (const std::exception& error) {
    std::cerr << "NebulaKV benchmark failed: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
