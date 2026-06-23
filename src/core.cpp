#include "industry_starter/core.hpp"

#include <numeric>

namespace industry_starter {

std::int64_t sum(const std::span<const int> values) noexcept {
  return std::accumulate(values.begin(), values.end(), std::int64_t{0});
}

} // namespace industry_starter
