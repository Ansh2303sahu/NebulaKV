#include "industry_starter/core.hpp"

#include <cstddef>
#include <vector>

#include <benchmark/benchmark.h>

static void benchmark_sum(benchmark::State& state) {
  const auto element_count = static_cast<std::size_t>(state.range(0));
  const std::vector<int> values(element_count, 1);

  for (auto _ : state) {
    (void)_;
    benchmark::DoNotOptimize(industry_starter::sum(values));
  }
}

BENCHMARK(benchmark_sum)->RangeMultiplier(8)->Range(8, 8 << 15);
