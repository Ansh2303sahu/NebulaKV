#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <benchmark/benchmark.h>
#include <unistd.h>

namespace {

[[nodiscard]] std::string compaction_key(const std::size_t index) {
  std::string digits = std::to_string(index);
  return "key-" + std::string(10U - digits.size(), '0') + digits;
}

[[nodiscard]] std::shared_ptr<nebulakv::MemTable> compaction_table(const std::uint64_t generation,
                                                                   const std::size_t key_count) {
  auto table = std::make_shared<nebulakv::MemTable>(generation);
  const std::uint64_t sequence_base = (generation - 1U) * static_cast<std::uint64_t>(key_count);
  for (std::size_t index = 0; index < key_count; ++index) {
    table->put(compaction_key(index), "value-" + std::to_string(generation) + std::string(64U, 'v'),
               sequence_base + static_cast<std::uint64_t>(index) + 1U);
  }
  table->freeze();
  return table;
}

void benchmark_level0_compaction(benchmark::State& state) {
  const std::size_t table_count = static_cast<std::size_t>(state.range(0));
  const std::size_t key_count = static_cast<std::size_t>(state.range(1));
  const auto directory = std::filesystem::temp_directory_path() /
                         ("nebulakv-compaction-benchmark-" + std::to_string(::getpid()) + "-" +
                          std::to_string(table_count) + "-" + std::to_string(key_count));

  nebulakv::CompactionResult last_result;
  for (auto _ : state) {
    static_cast<void>(_);
    state.PauseTiming();
    std::filesystem::remove_all(directory);
    nebulakv::SSTableManagerOptions options;
    options.directory = directory;
    options.target_data_block_bytes = 32U * 1024U;
    options.level0_compaction_trigger = 2U;
    options.level0_compaction_max_tables = table_count;
    auto manager = std::make_unique<nebulakv::SSTableManager>(options);
    for (std::size_t index = 0; index < table_count; ++index) {
      static_cast<void>(
          manager->flush(*compaction_table(static_cast<std::uint64_t>(index) + 1U, key_count)));
    }
    state.ResumeTiming();

    last_result = manager->compact_level0(true);
    benchmark::DoNotOptimize(last_result);
    benchmark::ClobberMemory();

    state.PauseTiming();
    manager.reset();
    std::filesystem::remove_all(directory);
    state.ResumeTiming();
  }

  const std::uint64_t entries_per_iteration =
      static_cast<std::uint64_t>(table_count) * static_cast<std::uint64_t>(key_count);
  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                          static_cast<std::int64_t>(entries_per_iteration));
  state.counters["input_tables"] = static_cast<double>(last_result.input_tables);
  state.counters["input_entries"] = static_cast<double>(last_result.input_entries);
  state.counters["output_entries"] = static_cast<double>(last_result.output_entries);
}

} // namespace

BENCHMARK(benchmark_level0_compaction)->Args({4, 1000})->Args({8, 1000})->Args({4, 10000});
