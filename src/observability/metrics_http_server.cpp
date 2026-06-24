#include "nebulakv/observability/metrics_http_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace nebulakv::observability {

namespace {

void send_all(const int descriptor, const std::string_view response) {
  std::size_t sent = 0U;
  while (sent < response.size()) {
    const auto result =
        ::send(descriptor, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    sent += static_cast<std::size_t>(result);
  }
}

} // namespace

MetricsHttpServer::MetricsHttpServer(std::string host, const std::uint16_t port, Renderer renderer)
    : host_{std::move(host)}, requested_port_{port}, renderer_{std::move(renderer)} {
  if (host_.empty()) {
    throw std::invalid_argument{"metrics host must not be empty"};
  }
  if (!renderer_) {
    throw std::invalid_argument{"metrics renderer must be provided"};
  }
}

MetricsHttpServer::~MetricsHttpServer() { stop(); }

void MetricsHttpServer::start() {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }
  stop_requested_.store(false, std::memory_order_release);
  thread_ = std::thread{&MetricsHttpServer::run, this};
  for (int attempt = 0; attempt < 100 && bound_port() == 0U; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  if (bound_port() == 0U) {
    stop();
    throw std::runtime_error{"metrics HTTP server failed to bind"};
  }
}

void MetricsHttpServer::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  stop_requested_.store(true, std::memory_order_release);
  if (listen_descriptor_ >= 0) {
    ::shutdown(listen_descriptor_, SHUT_RDWR);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  bound_port_.store(0U, std::memory_order_release);
}

std::uint16_t MetricsHttpServer::bound_port() const noexcept {
  return bound_port_.load(std::memory_order_acquire);
}

void MetricsHttpServer::run() {
  listen_descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_descriptor_ < 0) {
    return;
  }
  int reuse = 1;
  static_cast<void>(
      ::setsockopt(listen_descriptor_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(requested_port_);
  if (::inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
    ::close(listen_descriptor_);
    listen_descriptor_ = -1;
    return;
  }
  if (::bind(listen_descriptor_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::listen(listen_descriptor_, 16) != 0) {
    ::close(listen_descriptor_);
    listen_descriptor_ = -1;
    return;
  }

  socklen_t address_size = sizeof(address);
  if (::getsockname(listen_descriptor_, reinterpret_cast<sockaddr*>(&address), &address_size) ==
      0) {
    bound_port_.store(ntohs(address.sin_port), std::memory_order_release);
  }

  while (!stop_requested_.load(std::memory_order_acquire)) {
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(listen_descriptor_, &descriptors);
    timeval timeout{};
    timeout.tv_usec = 200'000;
    const int ready = ::select(listen_descriptor_ + 1, &descriptors, nullptr, nullptr, &timeout);
    if (ready <= 0) {
      continue;
    }
    const int client = ::accept(listen_descriptor_, nullptr, nullptr);
    if (client < 0) {
      continue;
    }
    char request[1024]{};
    const auto bytes = ::recv(client, request, sizeof(request), 0);
    const bool metrics_request =
        bytes > 0 &&
        std::string_view{request, static_cast<std::size_t>(bytes)}.starts_with("GET /metrics ");
    const std::string body = metrics_request ? renderer_() : "not found\n";
    const std::string status = metrics_request ? "200 OK" : "404 Not Found";
    const std::string response = "HTTP/1.1 " + status +
                                 "\r\nContent-Type: text/plain; version=0.0.4\r\n"
                                 "Connection: close\r\nContent-Length: " +
                                 std::to_string(body.size()) + "\r\n\r\n" + body;
    send_all(client, response);
    ::close(client);
  }

  if (listen_descriptor_ >= 0) {
    ::close(listen_descriptor_);
    listen_descriptor_ = -1;
  }
}

} // namespace nebulakv::observability
