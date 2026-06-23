#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_reader.hpp"
#include "nebulakv/sstable_writer.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

#include <benchmark/benchmark.h>
#include <unistd.h>

namespace {

[[nodiscard]] std::string benchmark_key(const std::size_t index) {
  std::string digits = std::to_string(index);
  return "key-" + std::string(10U - digits.size(), '0') + digits;
}

void benchmark_sstable_indexed_get(benchmark::State& state) {
  const std::size_t key_count = static_cast<std::size_t>(state.range(0));
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("nebulakv-benchmark-" + std::to_string(::getpid()) + "-" + std::to_string(key_count));
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);
  const auto path = directory / "indexed-read.sst";

  nebulakv::MemTable table{0U};
  for (std::size_t index = 0; index < key_count; ++index) {
    table.put(benchmark_key(index), std::string(128U, 'v'), index + 1U);
  }
  table.freeze();
  const auto metadata = nebulakv::SSTableWriter::write(
      table.snapshot(), nebulakv::SSTableWriterOptions{path, 32U * 1024U, 0U});
  const nebulakv::SSTableReader reader{path};
  const std::string target = benchmark_key(key_count / 2U);

  for (auto _ : state) {
    static_cast<void>(_);
    auto entry = reader.get(target);
    benchmark::DoNotOptimize(entry);
  }

  state.SetItemsProcessed(state.iterations());
  state.counters["blocks"] = static_cast<double>(metadata.block_count);
  state.counters["keys"] = static_cast<double>(key_count);
  std::filesystem::remove_all(directory);
}

} // namespace

BENCHMARK(benchmark_sstable_indexed_get)->Arg(1000)->Arg(10000)->Arg(100000);
