#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_manager.hpp"

#include "test_support.hpp"

#include <cstddef>
#include <memory>
#include <string>

#include <gtest/gtest.h>

namespace nebulakv {
namespace {

std::shared_ptr<MemTable> immutable_table(const std::uint64_t generation, const std::string& key,
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

std::shared_ptr<MemTable> immutable_table_with_even_keys(const std::uint64_t generation,
                                                         const std::size_t count) {
  auto table = std::make_shared<MemTable>(generation);
  for (std::size_t index = 0; index < count; ++index) {
    const std::string key = "key-" + std::to_string(index * 2U + 1000U);
    table->put(key, std::string(96U, 'v'), index + 1U);
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
  const BloomFilterAggregateStatistics statistics = reopened.bloom_filter_statistics();
  EXPECT_EQ(statistics.filter_count, 1U);
  EXPECT_EQ(statistics.inserted_keys, 100U);
  EXPECT_GT(statistics.memory_bytes, 0U);
}

} // namespace
} // namespace nebulakv
