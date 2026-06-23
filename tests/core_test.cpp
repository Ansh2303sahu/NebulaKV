#include "industry_starter/core.hpp"

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

TEST(SumTest, ReturnsZeroForEmptyInput) {
  constexpr std::array<int, 0> values{};
  EXPECT_EQ(industry_starter::sum(values), std::int64_t{0});
}

TEST(SumTest, AddsPositiveAndNegativeValues) {
  constexpr std::array values{5, -3, 8, -10};
  EXPECT_EQ(industry_starter::sum(values), std::int64_t{0});
}

TEST(SumTest, AccumulatesUsingSixtyFourBits) {
  constexpr std::array values{2'000'000'000, 2'000'000'000};
  EXPECT_EQ(industry_starter::sum(values), std::int64_t{4'000'000'000});
}
