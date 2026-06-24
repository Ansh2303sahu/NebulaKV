#include "nebulakv/block_cache.hpp"
#include "nebulakv/bloom_filter.hpp"
#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_reader.hpp"
#include "nebulakv/sstable_writer.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>
#include <unistd.h>

namespace {

[[nodiscard]] std::string benchmark_key(const std::size_t index) {
  std::string digits = std::to_string(index);
  return "key-" + std::string(10U - digits.size(), '0') + digits;
}

[[nodiscard]] std::filesystem::path benchmark_directory(
    const std::string_view name, const std::size_t key_count) {
  return std::filesystem::temp_directory_path() /
         ("nebulakv-benchmark-" + std::string{name} + "-" +
          std::to_string(::getpid()) + "-" + std::to_string(key_count));
}

[[nodiscard]] nebulakv::MemTable::Snapshot write_benchmark_table(
    const std::filesystem::path& path, const std::size_t key_count,
    const bool even_keys_only = false) {
  nebulakv::MemTable table{0U};
  for (std::size_t index = 0; index < key_count; ++index) {
    const std::size_t logical_index = even_keys_only ? index * 2U : index;
    table.put(benchmark_key(logical_index), std::string(128U, 'v'), index + 1U);
  }
  table.freeze();
  const auto snapshot = table.snapshot();
  static_cast<void>(nebulakv::SSTableWriter::write(
      snapshot, nebulakv::SSTableWriterOptions{path, 32U * 1024U, 0U}));
  return snapshot;
}

void benchmark_sstable_uncached_get(benchmark::State& state) {
  const std::size_t key_count = static_cast<std::size_t>(state.range(0));
  const auto directory = benchmark_directory("uncached", key_count);
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = directory / "table.sst";
  static_cast<void>(write_benchmark_table(path, key_count));
  const nebulakv::SSTableReader reader{path};
  const std::string target = benchmark_key(key_count / 2U);

  for (auto _ : state) {
    static_cast<void>(_);
    auto entry = reader.get(target);
    benchmark::DoNotOptimize(entry);
  }

  state.SetItemsProcessed(state.iterations());
  state.counters["blocks"] = static_cast<double>(reader.metadata().block_count);
  state.counters["keys"] = static_cast<double>(key_count);
  std::filesystem::remove_all(directory);
}

void benchmark_sstable_cached_get(benchmark::State& state) {
  const std::size_t key_count = static_cast<std::size_t>(state.range(0));
  const auto directory = benchmark_directory("cached", key_count);
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = directory / "table.sst";
  static_cast<void>(write_benchmark_table(path, key_count));
  auto cache = std::make_shared<nebulakv::BlockCache>(64U * 1024U * 1024U);
  const nebulakv::SSTableReader reader{path, cache};
  const std::string target = benchmark_key(key_count / 2U);
  benchmark::DoNotOptimize(reader.get(target));
  cache->reset_statistics();

  for (auto _ : state) {
    static_cast<void>(_);
    auto entry = reader.get(target);
    benchmark::DoNotOptimize(entry);
  }

  const nebulakv::BlockCacheStatistics statistics = cache->statistics();
  state.SetItemsProcessed(state.iterations());
  state.counters["cache_hit_ratio"] = statistics.hit_ratio();
  state.counters["cached_blocks"] = static_cast<double>(statistics.entry_count);
  state.counters["keys"] = static_cast<double>(key_count);
  std::filesystem::remove_all(directory);
}

void benchmark_sstable_bloom_negative_get(benchmark::State& state) {
  const std::size_t key_count = static_cast<std::size_t>(state.range(0));
  const auto directory = benchmark_directory("bloom-negative", key_count);
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = directory / "table.sst";
  const auto snapshot = write_benchmark_table(path, key_count, true);
  auto filter = std::make_shared<nebulakv::BloomFilter>(key_count, 0.01);
  for (const auto& [key, entry] : snapshot) {
    static_cast<void>(entry);
    filter->add(key);
  }
  auto cache = std::make_shared<nebulakv::BlockCache>(64U * 1024U * 1024U);
  const nebulakv::SSTableReader reader{path, cache, filter};
  const std::string missing = benchmark_key((key_count / 2U) * 2U + 1U);

  for (auto _ : state) {
    static_cast<void>(_);
    auto entry = reader.get(missing);
    benchmark::DoNotOptimize(entry);
  }

  const auto bloom = reader.lookup_statistics();
  const auto cache_statistics = cache->statistics();
  state.SetItemsProcessed(state.iterations());
  state.counters["block_reads"] =
      static_cast<double>(cache_statistics.misses);
  state.counters["bloom_negatives"] =
      static_cast<double>(bloom.bloom_negatives);
  state.counters["keys"] = static_cast<double>(key_count);
  std::filesystem::remove_all(directory);
}

}  // namespace

BENCHMARK(benchmark_sstable_uncached_get)->Arg(1000)->Arg(10000)->Arg(100000);
BENCHMARK(benchmark_sstable_cached_get)->Arg(1000)->Arg(10000)->Arg(100000);
BENCHMARK(benchmark_sstable_bloom_negative_get)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);
