#include "nebulakv/block_cache.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace nebulakv {
namespace {

[[nodiscard]] std::shared_ptr<const DataBlock> make_block(const std::string& key,
                                                          const std::string& value) {
  auto block = std::make_shared<DataBlock>();
  block->records.emplace_back(key, Entry{value, 1U, false});
  return block;
}

TEST(BlockCacheTest, RejectsZeroCapacity) { EXPECT_THROW(BlockCache(0U), std::invalid_argument); }

TEST(BlockCacheTest, RecordsMissThenHit) {
  const auto block = make_block("key", "value");
  BlockCache cache{BlockCache::estimate_charge(*block) * 2U};

  EXPECT_FALSE(cache.get("table", 10U));
  cache.put("table", 10U, block);
  EXPECT_EQ(cache.get("table", 10U), block);

  const BlockCacheStatistics statistics = cache.statistics();
  EXPECT_EQ(statistics.misses, 1U);
  EXPECT_EQ(statistics.hits, 1U);
  EXPECT_EQ(statistics.hit_ratio(), 0.5);
}

TEST(BlockCacheTest, EvictsLeastRecentlyUsedBlock) {
  const auto first = make_block("first", "value");
  const auto second = make_block("second", "value");
  const auto third = make_block("third", "value");
  const std::size_t capacity =
      BlockCache::estimate_charge(*first) + BlockCache::estimate_charge(*second);
  BlockCache cache{capacity};

  cache.put("table", 1U, first);
  cache.put("table", 2U, second);
  ASSERT_TRUE(cache.get("table", 1U));
  cache.put("table", 3U, third);

  EXPECT_TRUE(cache.get("table", 1U));
  EXPECT_FALSE(cache.get("table", 2U));
  EXPECT_TRUE(cache.get("table", 3U));
  EXPECT_EQ(cache.statistics().evictions, 1U);
}

TEST(BlockCacheTest, ReplacingEntryDoesNotGrowEntryCount) {
  const auto original = make_block("key", "one");
  const auto replacement = make_block("key", "two");
  BlockCache cache{4096U};

  cache.put("table", 1U, original);
  cache.put("table", 1U, replacement);

  EXPECT_EQ(cache.statistics().entry_count, 1U);
  EXPECT_EQ(cache.get("table", 1U), replacement);
}

TEST(BlockCacheTest, OversizedBlockIsNotCached) {
  const auto block = make_block("key", std::string(4096U, 'v'));
  BlockCache cache{128U};

  cache.put("table", 1U, block);

  EXPECT_FALSE(cache.get("table", 1U));
  EXPECT_EQ(cache.statistics().entry_count, 0U);
}

TEST(BlockCacheTest, ClearDropsEntriesButPreservesCounters) {
  const auto block = make_block("key", "value");
  BlockCache cache{4096U};
  cache.put("table", 1U, block);
  ASSERT_TRUE(cache.get("table", 1U));

  cache.clear();

  EXPECT_EQ(cache.statistics().entry_count, 0U);
  EXPECT_EQ(cache.statistics().hits, 1U);
}

TEST(BlockCacheTest, ResetStatisticsPreservesCachedEntries) {
  const auto block = make_block("key", "value");
  BlockCache cache{4096U};
  cache.put("table", 1U, block);
  ASSERT_TRUE(cache.get("table", 1U));

  cache.reset_statistics();

  EXPECT_EQ(cache.statistics().hits, 0U);
  EXPECT_TRUE(cache.get("table", 1U));
}

TEST(BlockCacheTest, SupportsConcurrentReads) {
  const auto block = make_block("key", "value");
  BlockCache cache{4096U};
  cache.put("table", 1U, block);

  constexpr std::size_t thread_count = 8U;
  constexpr std::size_t reads_per_thread = 1000U;
  std::atomic<bool> failed{false};
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (std::size_t thread = 0; thread < thread_count; ++thread) {
    threads.emplace_back([&cache, &block, &failed] {
      for (std::size_t read = 0; read < reads_per_thread; ++read) {
        if (cache.get("table", 1U) != block) {
          failed.store(true, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_FALSE(failed.load(std::memory_order_relaxed));
  EXPECT_EQ(cache.statistics().hits, thread_count * reads_per_thread);
}

} // namespace
} // namespace nebulakv
