#include "nebulakv/raft/storage.hpp"
#include "test_support.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace nebulakv::raft {

TEST(RaftStorageTest, PersistsHardStateLogAndSnapshot) {
  test::TemporaryDirectory directory;
  RaftStorage storage{directory.path() / "raft"};

  HardState hard_state{7U, "node-2", 2U, 2U};
  std::vector<LogEntry> log{{1U, 6U, {CommandType::Put, "alpha", "one"}},
                            {2U, 7U, {CommandType::Delete, "beta", {}}}};
  Snapshot snapshot{2U, 7U, "snapshot-payload"};

  storage.save_hard_state(hard_state);
  storage.replace_log(log);
  storage.save_snapshot(snapshot);

  const PersistentState loaded = storage.load();
  EXPECT_EQ(loaded.hard_state.current_term, 7U);
  EXPECT_EQ(loaded.hard_state.voted_for, "node-2");
  EXPECT_EQ(loaded.log, log);
  ASSERT_TRUE(loaded.snapshot.has_value());
  EXPECT_EQ(loaded.snapshot->payload, "snapshot-payload");
}

TEST(RaftStorageTest, DetectsCorruptedPersistentLog) {
  test::TemporaryDirectory directory;
  RaftStorage storage{directory.path() / "raft"};
  storage.replace_log({{1U, 1U, {CommandType::Put, "alpha", "one"}}});

  auto bytes = test::read_file(storage.log_path());
  ASSERT_GT(bytes.size(), 12U);
  bytes[10U] ^= std::byte{0x5A};
  test::write_file(storage.log_path(), bytes);

  EXPECT_THROW(static_cast<void>(storage.load()), std::runtime_error);
}

TEST(RaftStorageTest, AtomicallyReplacesLogContents) {
  test::TemporaryDirectory directory;
  RaftStorage storage{directory.path() / "raft"};
  storage.replace_log(
      {{1U, 1U, {CommandType::Put, "a", "1"}}, {2U, 1U, {CommandType::Put, "b", "2"}}});
  storage.replace_log({{2U, 1U, {CommandType::Put, "b", "2"}}});

  const auto loaded = storage.load();
  ASSERT_EQ(loaded.log.size(), 1U);
  EXPECT_EQ(loaded.log.front().index, 2U);
  EXPECT_FALSE(std::filesystem::exists(storage.log_path().string() + ".tmp"));
}

} // namespace nebulakv::raft
