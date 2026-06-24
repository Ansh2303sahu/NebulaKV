#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_manager.hpp"
#include "nebulakv/sstable_writer.hpp"

#include "test_support.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace nebulakv {
namespace {

std::shared_ptr<MemTable> immutable_table(const std::uint64_t generation,
                                          const std::string& key,
                                          const Entry& entry) {
  auto table = std::make_shared<MemTable>(generation);
  if (entry.deleted) {
    table->add_tombstone(key, entry.sequence_number);
  } else {
    table->put(key, entry.value, entry.sequence_number);
  }
  table->freeze();
  return table;
}

std::shared_ptr<MemTable> immutable_table_with_even_keys(
    const std::uint64_t generation, const std::size_t count) {
  auto table = std::make_shared<MemTable>(generation);
  for (std::size_t index = 0; index < count; ++index) {
    const std::string key = "key-" + std::to_string(index * 2U + 1000U);
    table->put(key, std::string(96U, 'v'), index + 1U);
  }
  table->freeze();
  return table;
}

std::shared_ptr<MemTable> immutable_table_with_range(
    const std::uint64_t generation, const std::string& prefix,
    const std::size_t count, const std::uint64_t sequence_base) {
  auto table = std::make_shared<MemTable>(generation);
  for (std::size_t index = 0; index < count; ++index) {
    const std::string key = prefix + std::to_string(100000U + index);
    table->put(key, "value-" + std::to_string(generation),
               sequence_base + index + 1U);
  }
  table->freeze();
  return table;
}


TEST(SSTableManagerTest, FlushesAndLoadsTableAfterRestart) {
  test::TemporaryDirectory directory;
  {
    SSTableManager manager{{directory.path(), 128U}};
    const auto table = immutable_table(1U, "key", Entry{"value", 5U, false});
    static_cast<void>(manager.flush(*table));
    EXPECT_EQ(manager.table_count(), 1U);
    EXPECT_EQ(manager.get("key")->value, "value");
  }

  const SSTableManager reopened{{directory.path(), 128U}};
  EXPECT_EQ(reopened.table_count(), 1U);
  EXPECT_EQ(reopened.get("key")->value, "value");
  EXPECT_EQ(reopened.live_key_count(), 1U);
  EXPECT_EQ(reopened.max_sequence_number(), 5U);
  EXPECT_EQ(reopened.next_generation(), 2U);
}

TEST(SSTableManagerTest, NewestSequenceWinsAcrossTables) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 128U}};
  static_cast<void>(manager.flush(*immutable_table(1U, "key", Entry{"old", 5U, false})));
  static_cast<void>(manager.flush(*immutable_table(2U, "key", Entry{"new", 8U, false})));

  EXPECT_EQ(manager.get("key")->value, "new");
  EXPECT_EQ(manager.live_key_count(), 1U);
}

TEST(SSTableManagerTest, NewerTombstoneHidesOlderValue) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 128U}};
  static_cast<void>(manager.flush(*immutable_table(1U, "key", Entry{"old", 5U, false})));
  static_cast<void>(manager.flush(*immutable_table(2U, "key", Entry{{}, 8U, true})));

  const auto entry = manager.get("key");
  ASSERT_TRUE(entry.has_value());
  EXPECT_TRUE(entry->deleted);
  EXPECT_EQ(manager.live_key_count(), 0U);
}

TEST(SSTableManagerTest, RejectsMutableMemTable) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 128U}};
  MemTable table{1U};
  table.put("key", "value", 1U);

  EXPECT_THROW(static_cast<void>(manager.flush(table)), std::invalid_argument);
}

TEST(SSTableManagerTest, RemovesAbandonedTemporaryFilesOnStartup) {
  test::TemporaryDirectory directory;
  const auto temporary = directory.file("sstable-1-1.sst.tmp");
  test::write_file(temporary, {std::byte{1}, std::byte{2}});

  const SSTableManager manager{{directory.path(), 128U}};

  EXPECT_EQ(manager.table_count(), 0U);
  EXPECT_FALSE(std::filesystem::exists(temporary));
}

TEST(SSTableManagerTest, BloomFilterSkipsMostMissingKeysWithoutBlockReads) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 1024U, 1024U * 1024U, 0.01}};
  static_cast<void>(manager.flush(*immutable_table_with_even_keys(1U, 500U)));
  manager.clear_block_cache();
  manager.reset_read_statistics();

  for (std::size_t index = 0; index < 100U; ++index) {
    const std::string key = "key-" + std::to_string(index * 2U + 1001U);
    EXPECT_FALSE(manager.get(key).has_value());
  }

  const BloomFilterAggregateStatistics bloom = manager.bloom_filter_statistics();
  const BlockCacheStatistics cache = manager.block_cache_statistics();
  EXPECT_EQ(bloom.filter_count, 1U);
  EXPECT_EQ(bloom.inserted_keys, 500U);
  EXPECT_GT(bloom.checks, 80U);
  EXPECT_GT(bloom.negative_results, 80U);
  EXPECT_LT(cache.misses, 10U);
}

TEST(SSTableManagerTest, RepeatedReadsUseSharedBlockCache) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 1024U, 1024U * 1024U, 0.01}};
  static_cast<void>(manager.flush(*immutable_table_with_even_keys(1U, 100U)));
  manager.clear_block_cache();
  manager.reset_read_statistics();

  ASSERT_TRUE(manager.get("key-1100").has_value());
  ASSERT_TRUE(manager.get("key-1100").has_value());

  const BlockCacheStatistics cache = manager.block_cache_statistics();
  EXPECT_EQ(cache.misses, 1U);
  EXPECT_EQ(cache.hits, 1U);
  EXPECT_EQ(cache.hit_ratio(), 0.5);
}

TEST(SSTableManagerTest, CacheEvictsBlocksWhenCapacityIsExceeded) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 1024U, 2500U, 0.01}};
  static_cast<void>(manager.flush(*immutable_table_with_even_keys(1U, 200U)));
  manager.clear_block_cache();
  manager.reset_read_statistics();

  ASSERT_TRUE(manager.get("key-1000").has_value());
  ASSERT_TRUE(manager.get("key-1200").has_value());
  ASSERT_TRUE(manager.get("key-1398").has_value());

  EXPECT_GT(manager.block_cache_statistics().evictions, 0U);
}

TEST(SSTableManagerTest, ReportsBloomFilterMemoryUsageAfterRestart) {
  test::TemporaryDirectory directory;
  {
    SSTableManager manager{{directory.path(), 256U}};
    static_cast<void>(manager.flush(*immutable_table_with_even_keys(1U, 100U)));
  }

  SSTableManager reopened{{directory.path(), 256U}};
  const BloomFilterAggregateStatistics statistics =
      reopened.bloom_filter_statistics();
  EXPECT_EQ(statistics.filter_count, 1U);
  EXPECT_EQ(statistics.inserted_keys, 100U);
  EXPECT_GT(statistics.memory_bytes, 0U);
}

TEST(SSTableManagerTest, CreatesCurrentAndVersionedManifest) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 128U}};

  EXPECT_TRUE(std::filesystem::exists(manager.current_path()));
  EXPECT_TRUE(std::filesystem::exists(manager.active_manifest_path()));
  EXPECT_EQ(manager.table_count(), 0U);
}

TEST(SSTableManagerTest, CompactsTriggeredLevelZeroTablesIntoLevelOne) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 128U}};
  static_cast<void>(manager.flush(*immutable_table(1U, "a", Entry{"old", 1U, false})));
  static_cast<void>(manager.flush(*immutable_table(2U, "b", Entry{"two", 2U, false})));
  static_cast<void>(manager.flush(*immutable_table(3U, "a", Entry{"new", 3U, false})));
  static_cast<void>(manager.flush(*immutable_table(4U, "c", Entry{"three", 4U, false})));

  EXPECT_TRUE(manager.needs_compaction());
  const auto before = manager.metadata();
  const CompactionResult result = manager.compact_if_needed();

  EXPECT_TRUE(result.performed);
  EXPECT_EQ(result.input_tables, 4U);
  EXPECT_EQ(result.output_tables, 1U);
  EXPECT_EQ(manager.level_table_count(SSTableLevel::Level0), 0U);
  EXPECT_EQ(manager.level_table_count(SSTableLevel::Level1), 1U);
  ASSERT_TRUE(manager.get("a").has_value());
  EXPECT_EQ(manager.get("a")->value, "new");
  for (const SSTableMetadata& table : before) {
    EXPECT_FALSE(std::filesystem::exists(table.path));
  }
}

TEST(SSTableManagerTest, ForcedCompactionWorksBelowAutomaticTrigger) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 128U}};
  static_cast<void>(manager.flush(*immutable_table(1U, "a", Entry{"one", 1U, false})));
  static_cast<void>(manager.flush(*immutable_table(2U, "b", Entry{"two", 2U, false})));

  EXPECT_FALSE(manager.compact_if_needed().performed);
  const CompactionResult result = manager.compact_level0(true);

  EXPECT_TRUE(result.performed);
  EXPECT_EQ(manager.level_table_count(SSTableLevel::Level0), 0U);
  EXPECT_EQ(manager.level_table_count(SSTableLevel::Level1), 1U);
}

TEST(SSTableManagerTest, CompactionDropsSafeTombstones) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 128U}};
  static_cast<void>(manager.flush(*immutable_table(1U, "key", Entry{"value", 1U, false})));
  static_cast<void>(manager.flush(*immutable_table(2U, "key", Entry{{}, 2U, true})));

  const CompactionResult result = manager.compact_level0(true);

  EXPECT_EQ(result.tombstones_dropped, 1U);
  EXPECT_EQ(result.output_tables, 0U);
  EXPECT_EQ(manager.table_count(), 0U);
  EXPECT_FALSE(manager.get("key").has_value());
  EXPECT_EQ(manager.live_key_count(), 0U);
}

TEST(SSTableManagerTest, OverlappingLevelOneTableParticipatesInNextCompaction) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 128U}};
  static_cast<void>(manager.flush(*immutable_table(1U, "a", Entry{"a1", 1U, false})));
  static_cast<void>(manager.flush(*immutable_table(2U, "b", Entry{"b1", 2U, false})));
  static_cast<void>(manager.flush(*immutable_table(3U, "c", Entry{"c1", 3U, false})));
  static_cast<void>(manager.flush(*immutable_table(4U, "d", Entry{"d1", 4U, false})));
  static_cast<void>(manager.compact_if_needed());

  static_cast<void>(manager.flush(*immutable_table(5U, "b", Entry{"b2", 5U, false})));
  static_cast<void>(manager.flush(*immutable_table(6U, "e", Entry{"e2", 6U, false})));
  static_cast<void>(manager.flush(*immutable_table(7U, "f", Entry{"f2", 7U, false})));
  static_cast<void>(manager.flush(*immutable_table(8U, "g", Entry{"g2", 8U, false})));
  const CompactionResult result = manager.compact_if_needed();

  EXPECT_EQ(result.input_tables, 5U);
  EXPECT_EQ(manager.level_table_count(SSTableLevel::Level1), 1U);
  ASSERT_TRUE(manager.get("b").has_value());
  EXPECT_EQ(manager.get("b")->value, "b2");
  EXPECT_EQ(manager.get("a")->value, "a1");
}

TEST(SSTableManagerTest, NonOverlappingCompactionsCreateSeparateLevelOneTables) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 128U}};
  for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
    static_cast<void>(manager.flush(*immutable_table(
        generation, "a" + std::to_string(generation),
        Entry{"left", generation, false})));
  }
  static_cast<void>(manager.compact_if_needed());

  for (std::uint64_t generation = 5U; generation <= 8U; ++generation) {
    static_cast<void>(manager.flush(*immutable_table(
        generation, "z" + std::to_string(generation),
        Entry{"right", generation, false})));
  }
  static_cast<void>(manager.compact_if_needed());

  EXPECT_EQ(manager.level_table_count(SSTableLevel::Level1), 2U);
  EXPECT_EQ(manager.level_table_count(SSTableLevel::Level0), 0U);
}

TEST(SSTableManagerTest, CompactedStateLoadsFromManifestAfterRestart) {
  test::TemporaryDirectory directory;
  {
    SSTableManager manager{{directory.path(), 128U}};
    for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
      static_cast<void>(manager.flush(*immutable_table(
          generation, "key", Entry{"value-" + std::to_string(generation),
                                    generation, false})));
    }
    static_cast<void>(manager.compact_if_needed());
  }

  SSTableManager reopened{{directory.path(), 128U}};
  EXPECT_EQ(reopened.level_table_count(SSTableLevel::Level1), 1U);
  EXPECT_EQ(reopened.level_table_count(SSTableLevel::Level0), 0U);
  ASSERT_TRUE(reopened.get("key").has_value());
  EXPECT_EQ(reopened.get("key")->value, "value-4");
}

TEST(SSTableManagerTest, ManifestIgnoresAndRemovesOrphanSSTable) {
  test::TemporaryDirectory directory;
  {
    SSTableManager manager{{directory.path(), 128U}};
    static_cast<void>(manager.flush(*immutable_table(
        1U, "key", Entry{"manifest-value", 1U, false})));
  }

  MemTable::Snapshot orphan_entries{{"key", Entry{"orphan-value", 100U, false}}};
  SSTableWriterOptions writer_options;
  writer_options.output_path = directory.file("orphan.sst");
  writer_options.target_data_block_bytes = 128U;
  writer_options.generation = 100U;
  static_cast<void>(SSTableWriter::write(orphan_entries, writer_options));
  ASSERT_TRUE(std::filesystem::exists(writer_options.output_path));

  SSTableManager reopened{{directory.path(), 128U}};
  EXPECT_EQ(reopened.get("key")->value, "manifest-value");
  EXPECT_FALSE(std::filesystem::exists(writer_options.output_path));
}

TEST(SSTableManagerTest, MissingManifestTableFailsStartup) {
  test::TemporaryDirectory directory;
  std::filesystem::path active_table;
  {
    SSTableManager manager{{directory.path(), 128U}};
    static_cast<void>(manager.flush(*immutable_table(
        1U, "key", Entry{"value", 1U, false})));
    active_table = manager.metadata().front().path;
  }
  ASSERT_TRUE(std::filesystem::remove(active_table));

  EXPECT_THROW((SSTableManager{SSTableManagerOptions{directory.path(), 128U}}),
               std::runtime_error);
}



TEST(SSTableManagerTest, MissingCurrentWithExistingManifestFailsStartup) {
  test::TemporaryDirectory directory;
  std::filesystem::path current;
  {
    SSTableManager manager{{directory.path(), 128U}};
    current = manager.current_path();
  }
  ASSERT_TRUE(std::filesystem::remove(current));

  EXPECT_THROW((SSTableManager{SSTableManagerOptions{directory.path(), 128U}}),
               std::runtime_error);
}

TEST(SSTableManagerTest, CorruptedCurrentFailsStartup) {
  test::TemporaryDirectory directory;
  std::filesystem::path current;
  {
    SSTableManager manager{{directory.path(), 128U}};
    current = manager.current_path();
  }
  auto bytes = test::read_file(current);
  ASSERT_TRUE(!bytes.empty());
  bytes.back() ^= std::byte{0xFF};
  test::write_file(current, bytes);

  EXPECT_THROW((SSTableManager{SSTableManagerOptions{directory.path(), 128U}}),
               std::runtime_error);
}

TEST(SSTableManagerTest, CorruptedManifestFailsStartup) {
  test::TemporaryDirectory directory;
  std::filesystem::path manifest;
  {
    SSTableManager manager{{directory.path(), 128U}};
    manifest = manager.active_manifest_path();
  }
  auto bytes = test::read_file(manifest);
  ASSERT_TRUE(!bytes.empty());
  bytes.back() ^= std::byte{0xAA};
  test::write_file(manifest, bytes);

  EXPECT_THROW((SSTableManager{SSTableManagerOptions{directory.path(), 128U}}),
               std::runtime_error);
}

TEST(SSTableManagerTest, MigratesLegacyDirectoryIntoManifest) {
  test::TemporaryDirectory directory;
  MemTable::Snapshot entries{{"legacy", Entry{"value", 1U, false}}};
  SSTableWriterOptions writer_options;
  writer_options.output_path = directory.file("legacy.sst");
  writer_options.target_data_block_bytes = 128U;
  writer_options.generation = 1U;
  static_cast<void>(SSTableWriter::write(entries, writer_options));

  SSTableManager manager{{directory.path(), 128U}};

  EXPECT_TRUE(std::filesystem::exists(manager.current_path()));
  EXPECT_TRUE(std::filesystem::exists(manager.active_manifest_path()));
  EXPECT_EQ(manager.level_table_count(SSTableLevel::Level0), 1U);
  EXPECT_EQ(manager.get("legacy")->value, "value");
}

TEST(SSTableManagerTest, ConcurrentReadsRemainAvailableDuringCompaction) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 1024U}};
  for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
    static_cast<void>(manager.flush(*immutable_table_with_range(
        generation, "key-", 3000U, (generation - 1U) * 3000U)));
  }

  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::thread reader{[&] {
    while (!stop.load(std::memory_order_acquire)) {
      try {
        const auto value = manager.get("key-101500");
        if (!value || value->value != "value-4") {
          failed.store(true, std::memory_order_release);
          return;
        }
      } catch (...) {
        failed.store(true, std::memory_order_release);
        return;
      }
    }
  }};

  const CompactionResult result = manager.compact_if_needed();
  stop.store(true, std::memory_order_release);
  reader.join();

  EXPECT_TRUE(result.performed);
  EXPECT_FALSE(failed.load(std::memory_order_acquire));
  EXPECT_EQ(manager.get("key-101500")->value, "value-4");
}

TEST(SSTableManagerTest, ReportsCompactionStatistics) {
  test::TemporaryDirectory directory;
  SSTableManager manager{{directory.path(), 128U}};
  for (std::uint64_t generation = 1U; generation <= 4U; ++generation) {
    static_cast<void>(manager.flush(*immutable_table(
        generation, "key-" + std::to_string(generation),
        Entry{"value", generation, false})));
  }
  const CompactionResult result = manager.compact_if_needed();
  const CompactionStatistics statistics = manager.compaction_statistics();

  EXPECT_TRUE(result.performed);
  EXPECT_EQ(statistics.runs, 1U);
  EXPECT_EQ(statistics.input_tables, result.input_tables);
  EXPECT_EQ(statistics.output_tables, result.output_tables);
  EXPECT_EQ(statistics.input_entries, result.input_entries);
  EXPECT_EQ(statistics.output_entries, result.output_entries);
}

}  // namespace
}  // namespace nebulakv
