#include "nebulakv/memtable.hpp"
#include "nebulakv/sstable_manager.hpp"

#include "test_support.hpp"

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

} // namespace
} // namespace nebulakv
