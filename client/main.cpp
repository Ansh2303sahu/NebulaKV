#include "nebulakv/network/grpc_client.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct Arguments {
  std::string host{"127.0.0.1"};
  std::uint16_t port{5001U};
  std::chrono::milliseconds timeout{2000};
  std::size_t max_message_bytes{8U * 1024U * 1024U};
  std::vector<std::string> command;
};

[[noreturn]] void usage(const char* program, const int exit_code) {
  std::ostream& output = exit_code == 0 ? std::cout : std::cerr;
  output << "Usage: " << program << " [options] <command> [arguments]\n"
         << "Options:\n"
         << "  --host <address>             default: 127.0.0.1\n"
         << "  --port <port>                default: 5001\n"
         << "  --timeout-ms <milliseconds>  default: 2000\n"
         << "  --max-message-bytes <bytes>  default: 8388608\n"
         << "Commands:\n"
         << "  put <key> <value>\n"
         << "  get <key>\n"
         << "  delete <key>\n"
         << "  batch-put <key> <value> [<key> <value> ...]\n"
         << "  status\n";
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

[[nodiscard]] Arguments parse_arguments(const int argc, char** argv) {
  Arguments arguments;
  int index = 1;
  for (; index < argc; ++index) {
    const std::string_view option{argv[index]};
    if (option == "--help" || option == "-h") {
      usage(argv[0], 0);
    }
    if (!option.starts_with("--")) {
      break;
    }
    if (option == "--host") {
      arguments.host = require_value(argc, argv, index, option);
    } else if (option == "--port") {
      arguments.port = parse_integer<std::uint16_t>(
          require_value(argc, argv, index, option), option);
    } else if (option == "--timeout-ms") {
      const auto milliseconds = parse_integer<std::int64_t>(
          require_value(argc, argv, index, option), option);
      arguments.timeout = std::chrono::milliseconds{milliseconds};
    } else if (option == "--max-message-bytes") {
      arguments.max_message_bytes = parse_integer<std::size_t>(
          require_value(argc, argv, index, option), option);
    } else {
      throw std::invalid_argument{"unknown option: " + std::string{option}};
    }
  }

  for (; index < argc; ++index) {
    arguments.command.emplace_back(argv[index]);
  }
  if (arguments.command.empty()) {
    throw std::invalid_argument{"a command is required"};
  }
  return arguments;
}

[[nodiscard]] int report_rpc_error(const grpc::Status& status) {
  std::cerr << "rpc_error_code=" << status.error_code() << '\n'
            << "rpc_error_message=" << status.error_message() << '\n';
  return 2;
}

}  // namespace

int main(const int argc, char** argv) {
  try {
    const Arguments arguments = parse_arguments(argc, argv);
    nebulakv::network::GrpcClientOptions options;
    options.address = arguments.host + ":" + std::to_string(arguments.port);
    options.timeout = arguments.timeout;
    options.max_message_bytes = arguments.max_message_bytes;
    const nebulakv::network::GrpcClient client{std::move(options)};

    const std::string& operation = arguments.command.front();
    if (operation == "put") {
      if (arguments.command.size() != 3U) {
        throw std::invalid_argument{"put requires a key and value"};
      }
      nebulakv::v1::PutResponse response;
      const grpc::Status status = client.put(
          arguments.command[1], arguments.command[2], response);
      if (!status.ok()) {
        return report_rpc_error(status);
      }
      std::cout << "success=" << response.success() << '\n'
                << "sequence_number=" << response.sequence_number() << '\n';
      return response.success() ? 0 : 3;
    }

    if (operation == "get") {
      if (arguments.command.size() != 2U) {
        throw std::invalid_argument{"get requires a key"};
      }
      nebulakv::v1::GetResponse response;
      const grpc::Status status = client.get(arguments.command[1], response);
      if (!status.ok()) {
        return report_rpc_error(status);
      }
      std::cout << "found=" << response.found() << '\n';
      if (response.found()) {
        std::cout << "value=" << response.value() << '\n';
      }
      return response.found() ? 0 : 4;
    }

    if (operation == "delete") {
      if (arguments.command.size() != 2U) {
        throw std::invalid_argument{"delete requires a key"};
      }
      nebulakv::v1::DeleteResponse response;
      const grpc::Status status = client.remove(arguments.command[1], response);
      if (!status.ok()) {
        return report_rpc_error(status);
      }
      std::cout << "deleted=" << response.deleted() << '\n';
      return response.deleted() ? 0 : 4;
    }

    if (operation == "batch-put") {
      if (arguments.command.size() < 3U ||
          (arguments.command.size() - 1U) % 2U != 0U) {
        throw std::invalid_argument{
            "batch-put requires one or more key/value pairs"};
      }
      std::vector<std::pair<std::string, std::string>> entries;
      for (std::size_t index = 1U; index < arguments.command.size(); index += 2U) {
        entries.emplace_back(arguments.command[index],
                             arguments.command[index + 1U]);
      }
      nebulakv::v1::BatchPutResponse response;
      const grpc::Status status = client.batch_put(std::move(entries), response);
      if (!status.ok()) {
        return report_rpc_error(status);
      }
      std::cout << "success=" << response.success() << '\n'
                << "writes_applied=" << response.writes_applied() << '\n'
                << "last_sequence_number=" << response.last_sequence_number()
                << '\n';
      return response.success() ? 0 : 3;
    }

    if (operation == "status") {
      if (arguments.command.size() != 1U) {
        throw std::invalid_argument{"status does not accept arguments"};
      }
      nebulakv::v1::StatusResponse response;
      const grpc::Status status = client.status(response);
      if (!status.ok()) {
        return report_rpc_error(status);
      }
      std::cout << "ready=" << response.ready() << '\n'
                << "live_keys=" << response.live_keys() << '\n'
                << "last_sequence_number=" << response.last_sequence_number()
                << '\n'
                << "level0_sstables=" << response.level0_sstables() << '\n'
                << "level1_sstables=" << response.level1_sstables() << '\n'
                << "cache_hit_ratio=" << response.cache_hit_ratio() << '\n'
                << "queued_requests=" << response.queued_requests() << '\n'
                << "active_requests=" << response.active_requests() << '\n'
                << "rejected_requests=" << response.rejected_requests() << '\n'
                << "requests_total=" << response.requests_total() << '\n'
                << "failed_requests_total="
                << response.failed_requests_total() << '\n';
      return response.ready() ? 0 : 5;
    }

    throw std::invalid_argument{"unknown command: " + operation};
  } catch (const std::exception& error) {
    std::cerr << "NebulaKV client failed: " << error.what() << '\n';
    return 1;
  }
}
