#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nebulakv::observability {

class MetricsRegistry final {
public:
  void increment(std::string_view name, std::uint64_t amount = 1U);
  void set_gauge(std::string_view name, double value);
  void observe(std::string_view name, double value);

  [[nodiscard]] std::uint64_t counter(std::string_view name) const;
  [[nodiscard]] double gauge(std::string_view name) const;
  [[nodiscard]] std::string render_prometheus() const;

private:
  struct Histogram {
    std::uint64_t count{0};
    double sum{0.0};
    std::vector<double> samples;
  };

  mutable std::mutex mutex_;
  std::map<std::string, std::uint64_t, std::less<>> counters_;
  std::map<std::string, double, std::less<>> gauges_;
  std::map<std::string, Histogram, std::less<>> histograms_;
};

} // namespace nebulakv::observability
