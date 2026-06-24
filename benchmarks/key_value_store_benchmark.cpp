#include "nebulakv/in_memory_key_value_store.hpp"
#include "nebulakv/memtable.hpp"
#include "nebulakv/memtable_set.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include <benchmark/benchmark.h>

namespace {

constexpr std::size_t kNoRotationLimit = std::numeric_limits<std::size_t>::max();

[[nodiscard]] std::size_t value_size(const benchmark::State& state) {
  return static_cast<std::size_t>(state.range(0));
}

void report_bytes(benchmark::State& state, const std::size_t bytes_per_iteration) {
  state.SetBytesProcessed(state.iterations() *
                          static_cast<std::int64_t>(bytes_per_iteration));
}

void benchmark_hash_store_get(benchmark::State& state) {
  nebulakv::InMemoryKeyValueStore store;
  const std::size_t bytes = value_size(state);
  store.put("benchmark-key", std::string(bytes, 'v'));

  for (auto _ : state) {
    (void)_;
    auto value = store.get("benchmark-key");
    benchmark::DoNotOptimize(value);
  }

  report_bytes(state, bytes);
}

void benchmark_hash_store_put_update(benchmark::State& state) {
  nebulakv::InMemoryKeyValueStore store;
  const std::size_t bytes = value_size(state);
  const std::string value(bytes, 'v');

  for (auto _ : state) {
    (void)_;
    store.put("benchmark-key", value);
    benchmark::ClobberMemory();
  }

  report_bytes(state, bytes);
}

void benchmark_sorted_memtable_get(benchmark::State& state) {
  nebulakv::MemTable table{0};
  const std::size_t bytes = value_size(state);
  table.put("benchmark-key", std::string(bytes, 'v'), 1U);

  for (auto _ : state) {
    (void)_;
    auto entry = table.get_entry("benchmark-key");
    benchmark::DoNotOptimize(entry);
  }

  report_bytes(state, bytes);
}

void benchmark_sorted_memtable_put_update(benchmark::State& state) {
  nebulakv::MemTable table{0};
  const std::size_t bytes = value_size(state);
  const std::string value(bytes, 'v');
  std::uint64_t sequence_number = 0U;

  for (auto _ : state) {
    (void)_;
    ++sequence_number;
    table.put("benchmark-key", value, sequence_number);
    benchmark::ClobberMemory();
  }

  report_bytes(state, bytes);
}

void benchmark_memtable_set_get(benchmark::State& state) {
  nebulakv::MemTableSet store{
      nebulakv::MemTableOptions{.max_memory_bytes = kNoRotationLimit}};
  const std::size_t bytes = value_size(state);
  store.put("benchmark-key", std::string(bytes, 'v'));

  for (auto _ : state) {
    (void)_;
    auto value = store.get("benchmark-key");
    benchmark::DoNotOptimize(value);
  }

  report_bytes(state, bytes);
  state.counters["immutable_tables"] =
      static_cast<double>(store.immutable_table_count());
  state.counters["active_bytes"] = static_cast<double>(store.active_memory_usage());
}

void benchmark_memtable_set_put_update(benchmark::State& state) {
  nebulakv::MemTableSet store{
      nebulakv::MemTableOptions{.max_memory_bytes = kNoRotationLimit}};
  const std::size_t bytes = value_size(state);
  const std::string value(bytes, 'v');

  for (auto _ : state) {
    (void)_;
    store.put("benchmark-key", value);
    benchmark::ClobberMemory();
  }

  report_bytes(state, bytes);
  state.counters["immutable_tables"] =
      static_cast<double>(store.immutable_table_count());
  state.counters["active_bytes"] = static_cast<double>(store.active_memory_usage());
}

}  // namespace

BENCHMARK(benchmark_hash_store_get)->RangeMultiplier(8)->Range(8, 8 << 15);
BENCHMARK(benchmark_hash_store_put_update)->RangeMultiplier(8)->Range(8, 8 << 15);
BENCHMARK(benchmark_sorted_memtable_get)->RangeMultiplier(8)->Range(8, 8 << 15);
BENCHMARK(benchmark_sorted_memtable_put_update)
    ->RangeMultiplier(8)
    ->Range(8, 8 << 15);
BENCHMARK(benchmark_memtable_set_get)->RangeMultiplier(8)->Range(8, 8 << 15);
BENCHMARK(benchmark_memtable_set_put_update)
    ->RangeMultiplier(8)
    ->Range(8, 8 << 15);
