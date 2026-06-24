#include "nebulakv/observability/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace nebulakv::observability {

namespace {

[[nodiscard]] double quantile(std::vector<double> values, const double q) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double position = q * static_cast<double>(values.size() - 1U);
  const auto lower = static_cast<std::size_t>(std::floor(position));
  const auto upper = static_cast<std::size_t>(std::ceil(position));
  if (lower == upper) {
    return values[lower];
  }
  const double fraction = position - static_cast<double>(lower);
  return values[lower] + ((values[upper] - values[lower]) * fraction);
}

} // namespace

void MetricsRegistry::increment(const std::string_view name, const std::uint64_t amount) {
  std::lock_guard lock{mutex_};
  counters_[std::string{name}] += amount;
}

void MetricsRegistry::set_gauge(const std::string_view name, const double value) {
  std::lock_guard lock{mutex_};
  gauges_.insert_or_assign(std::string{name}, value);
}

void MetricsRegistry::observe(const std::string_view name, const double value) {
  std::lock_guard lock{mutex_};
  auto& histogram = histograms_[std::string{name}];
  ++histogram.count;
  histogram.sum += value;
  constexpr std::size_t kMaximumSamples = 10'000U;
  if (histogram.samples.size() < kMaximumSamples) {
    histogram.samples.push_back(value);
  } else {
    const auto replacement = static_cast<std::size_t>(histogram.count % kMaximumSamples);
    histogram.samples[replacement] = value;
  }
}

std::uint64_t MetricsRegistry::counter(const std::string_view name) const {
  std::lock_guard lock{mutex_};
  const auto iterator = counters_.find(name);
  return iterator == counters_.end() ? 0U : iterator->second;
}

double MetricsRegistry::gauge(const std::string_view name) const {
  std::lock_guard lock{mutex_};
  const auto iterator = gauges_.find(name);
  return iterator == gauges_.end() ? 0.0 : iterator->second;
}

std::string MetricsRegistry::render_prometheus() const {
  std::lock_guard lock{mutex_};
  std::ostringstream output;
  output << std::setprecision(12);
  for (const auto& [name, value] : counters_) {
    output << name << ' ' << value << '\n';
  }
  for (const auto& [name, value] : gauges_) {
    output << name << ' ' << value << '\n';
  }
  for (const auto& [name, histogram] : histograms_) {
    output << name << "_count " << histogram.count << '\n'
           << name << "_sum " << histogram.sum << '\n'
           << name << "{quantile=\"0.5\"} " << quantile(histogram.samples, 0.5) << '\n'
           << name << "{quantile=\"0.95\"} " << quantile(histogram.samples, 0.95) << '\n'
           << name << "{quantile=\"0.99\"} " << quantile(histogram.samples, 0.99) << '\n';
  }
  return output.str();
}

} // namespace nebulakv::observability
