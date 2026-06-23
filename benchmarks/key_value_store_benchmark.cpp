#include "nebulakv/in_memory_key_value_store.hpp"

#include <cstddef>
#include <string>

#include <benchmark/benchmark.h>

namespace {

void benchmark_get(benchmark::State& state) {
  nebulakv::InMemoryKeyValueStore store;
  store.put("benchmark-key", std::string(static_cast<std::size_t>(state.range(0)), 'v'));

  for (auto _ : state) {
    (void)_;
    auto value = store.get("benchmark-key");
    benchmark::DoNotOptimize(value);
  }
}

void benchmark_put_update(benchmark::State& state) {
  nebulakv::InMemoryKeyValueStore store;
  const std::string value(static_cast<std::size_t>(state.range(0)), 'v');

  for (auto _ : state) {
    (void)_;
    store.put("benchmark-key", value);
    benchmark::ClobberMemory();
  }
}

} // namespace

BENCHMARK(benchmark_get)->RangeMultiplier(8)->Range(8, 8 << 15);
BENCHMARK(benchmark_put_update)->RangeMultiplier(8)->Range(8, 8 << 15);
