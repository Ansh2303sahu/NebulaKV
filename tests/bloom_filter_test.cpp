#include "nebulakv/bloom_filter.hpp"

#include <cstddef>
#include <string>

#include <gtest/gtest.h>

namespace nebulakv {
namespace {

TEST(BloomFilterTest, RejectsZeroExpectedEntries) {
  EXPECT_THROW(BloomFilter(0U, 0.01), std::invalid_argument);
}

TEST(BloomFilterTest, RejectsInvalidFalsePositiveRates) {
  EXPECT_THROW(BloomFilter(10U, 0.0), std::invalid_argument);
  EXPECT_THROW(BloomFilter(10U, 1.0), std::invalid_argument);
  EXPECT_THROW(BloomFilter(10U, -0.1), std::invalid_argument);
}

TEST(BloomFilterTest, NeverRejectsInsertedKeys) {
  BloomFilter filter{5000U, 0.01};
  for (std::size_t index = 0; index < 5000U; ++index) {
    filter.add("key-" + std::to_string(index));
  }

  for (std::size_t index = 0; index < 5000U; ++index) {
    EXPECT_TRUE(filter.may_contain("key-" + std::to_string(index)));
  }
}

TEST(BloomFilterTest, SupportsEmbeddedNullBytes) {
  BloomFilter filter{10U, 0.01};
  const std::string key{"a\0b\0c", 5U};

  filter.add(key);

  EXPECT_TRUE(filter.may_contain(key));
}

TEST(BloomFilterTest, ReportsMemoryAndInsertionStatistics) {
  BloomFilter filter{1000U, 0.01};
  filter.add("alpha");
  filter.add("beta");

  const BloomFilterStatistics statistics = filter.statistics();
  EXPECT_EQ(statistics.inserted_keys, 2U);
  EXPECT_GT(statistics.bit_count, 0U);
  EXPECT_GT(statistics.hash_count, 0U);
  EXPECT_GT(statistics.memory_bytes, 0U);
  EXPECT_EQ(statistics.target_false_positive_rate, 0.01);
}

TEST(BloomFilterTest, ObservedFalsePositiveRateStaysNearConfiguredTarget) {
  constexpr std::size_t inserted = 10000U;
  constexpr std::size_t probes = 10000U;
  BloomFilter filter{inserted, 0.01};
  for (std::size_t index = 0; index < inserted; ++index) {
    filter.add("present-" + std::to_string(index));
  }

  std::size_t false_positives = 0U;
  for (std::size_t index = 0; index < probes; ++index) {
    if (filter.may_contain("absent-" + std::to_string(index))) {
      ++false_positives;
    }
  }

  const double observed = static_cast<double>(false_positives) / static_cast<double>(probes);
  EXPECT_LT(observed, 0.03);
}

} // namespace
} // namespace nebulakv
