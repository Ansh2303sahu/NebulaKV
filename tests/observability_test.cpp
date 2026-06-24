#include "nebulakv/observability/json_logger.hpp"
#include "nebulakv/observability/metrics.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace nebulakv::observability {

TEST(ObservabilityTest, RendersCountersGaugesAndLatencyQuantiles) {
  MetricsRegistry registry;
  registry.increment("requests_total", 3U);
  registry.set_gauge("raft_term", 7.0);
  registry.observe("request_latency_seconds", 0.001);
  registry.observe("request_latency_seconds", 0.010);

  const std::string rendered = registry.render_prometheus();
  EXPECT_NE(rendered.find("requests_total 3"), std::string::npos);
  EXPECT_NE(rendered.find("raft_term 7"), std::string::npos);
  EXPECT_NE(rendered.find("request_latency_seconds_count 2"), std::string::npos);
  EXPECT_NE(rendered.find("quantile=\"0.99\""), std::string::npos);
}

TEST(ObservabilityTest, JsonLoggerEscapesStructuredFields) {
  std::ostringstream output;
  JsonLogger logger{output};
  logger.log("info", "leader_change", {{"node_id", "node-1"}, {"message", "line\nquote\""}});

  const std::string line = output.str();
  EXPECT_NE(line.find("\"event\":\"leader_change\""), std::string::npos);
  EXPECT_NE(line.find("line\\nquote\\\""), std::string::npos);
}

} // namespace nebulakv::observability
