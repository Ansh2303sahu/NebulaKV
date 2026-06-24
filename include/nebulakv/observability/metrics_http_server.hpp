#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace nebulakv::observability {

class MetricsHttpServer final {
public:
  using Renderer = std::function<std::string()>;

  MetricsHttpServer(std::string host, std::uint16_t port, Renderer renderer);
  ~MetricsHttpServer();

  MetricsHttpServer(const MetricsHttpServer&) = delete;
  MetricsHttpServer& operator=(const MetricsHttpServer&) = delete;

  void start();
  void stop();
  [[nodiscard]] std::uint16_t bound_port() const noexcept;

private:
  void run();

  std::string host_;
  std::uint16_t requested_port_{0};
  Renderer renderer_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint16_t> bound_port_{0};
  int listen_descriptor_{-1};
};

} // namespace nebulakv::observability
