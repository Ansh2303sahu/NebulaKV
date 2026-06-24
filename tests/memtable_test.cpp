#include "nebulakv/memtable.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

namespace {

using nebulakv::MemTable;

TEST(MemTableTest, StoresAndRetrievesEntry) {
  MemTable table{7};

  table.put("key", "value", 1);

  const auto entry = table.get_entry("key");
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->value, "value");
  EXPECT_EQ(entry->sequence_number, 1U);
  EXPECT_FALSE(entry->deleted);
  EXPECT_EQ(table.generation(), 7U);
}

TEST(MemTableTest, SnapshotIsSortedByKey) {
  MemTable table{0};
  table.put("charlie", "3", 1);
  table.put("alpha", "1", 2);
  table.put("bravo", "2", 3);

  const auto snapshot = table.snapshot();

  ASSERT_EQ(snapshot.size(), 3U);
  EXPECT_EQ(snapshot[0].first, "alpha");
  EXPECT_EQ(snapshot[1].first, "bravo");
  EXPECT_EQ(snapshot[2].first, "charlie");
}

TEST(MemTableTest, UpdateReplacesValueWithNewerSequence) {
  MemTable table{0};
  table.put("key", "old", 1);

  table.put("key", "new", 2);

  const auto entry = table.get_entry("key");
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->value, "new");
  EXPECT_EQ(entry->sequence_number, 2U);
  EXPECT_EQ(table.entry_count(), 1U);
}

TEST(MemTableTest, RejectsNonIncreasingSequenceForSameKey) {
  MemTable table{0};
  table.put("key", "value", 5);

  EXPECT_THROW(table.put("key", "older", 4), std::invalid_argument);
  EXPECT_THROW(table.put("key", "same", 5), std::invalid_argument);
}

TEST(MemTableTest, RejectsZeroSequenceNumber) {
  MemTable table{0};

  EXPECT_THROW(table.put("key", "value", 0), std::invalid_argument);
  EXPECT_THROW(table.add_tombstone("key", 0), std::invalid_argument);
}

TEST(MemTableTest, TombstoneRetainsKeyMetadata) {
  MemTable table{0};
  table.put("key", "value", 1);

  table.add_tombstone("key", 2);

  const auto entry = table.get_entry("key");
  ASSERT_TRUE(entry.has_value());
  EXPECT_TRUE(entry->deleted);
  EXPECT_TRUE(entry->value.empty());
  EXPECT_EQ(entry->sequence_number, 2U);
  EXPECT_EQ(table.entry_count(), 1U);
}

TEST(MemTableTest, LookupStateDistinguishesValuesTombstonesAndMissingKeys) {
  MemTable table{0};

  EXPECT_FALSE(table.lookup_state("missing").has_value());

  table.put("key", "value", 1);
  EXPECT_EQ(table.lookup_state("key"), MemTable::LookupState::Value);

  table.add_tombstone("key", 2);
  EXPECT_EQ(table.lookup_state("key"), MemTable::LookupState::Tombstone);
}

TEST(MemTableTest, FreezeMakesTableImmutable) {
  MemTable table{0};
  table.put("key", "value", 1);

  table.freeze();

  EXPECT_TRUE(table.is_immutable());
  EXPECT_THROW(table.put("other", "value", 2), std::logic_error);
  EXPECT_THROW(table.add_tombstone("key", 2), std::logic_error);
  EXPECT_EQ(table.get_entry("key")->value, "value");
}

TEST(MemTableTest, FreezingEmptyTableIsSafe) {
  MemTable table{0};

  table.freeze();
  table.freeze();

  EXPECT_TRUE(table.is_immutable());
  EXPECT_TRUE(table.empty());
}

TEST(MemTableTest, ReportsApproximateMemoryUsage) {
  MemTable table{0};
  const auto empty_usage = table.approximate_memory_usage();

  table.put("key", std::string(512, 'v'), 1);

  EXPECT_EQ(empty_usage, 0U);
  EXPECT_GT(table.approximate_memory_usage(), 512U);
}

TEST(MemTableTest, UpdatingValueRefreshesMemoryAccounting) {
  MemTable table{0};
  table.put("key", std::string(16, 'v'), 1);
  const auto small_usage = table.approximate_memory_usage();

  table.put("key", std::string(1024, 'v'), 2);
  const auto large_usage = table.approximate_memory_usage();

  table.add_tombstone("key", 3);
  const auto tombstone_usage = table.approximate_memory_usage();

  EXPECT_GT(large_usage, small_usage);
  EXPECT_LT(tombstone_usage, large_usage);
}

TEST(MemTableTest, SupportsEmbeddedNullBytes) {
  MemTable table{0};
  const std::string key{"key\0suffix", 10};
  const std::string value{"value\0suffix", 12};

  table.put(key, value, 1);

  const auto entry = table.get_entry(std::string_view{key.data(), key.size()});
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(entry->value, value);
}

TEST(MemTableTest, ValidatesKeysAndValues) {
  MemTable table{0};
  const std::string oversized_key(1025, 'k');
  const std::string oversized_value((1024U * 1024U) + 1U, 'v');

  EXPECT_THROW(table.put("", "value", 1), std::invalid_argument);
  EXPECT_THROW(table.put(oversized_key, "value", 1), std::length_error);
  EXPECT_THROW(table.put("key", oversized_value, 1), std::length_error);
  EXPECT_THROW(static_cast<void>(table.get_entry("")), std::invalid_argument);
}

}  // namespace
