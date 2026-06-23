#include "nebulakv/durability_mode.hpp"
#include "nebulakv/persistent_key_value_store.hpp"
#include "nebulakv/sstable_error.hpp"
#include "test_support.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#ifndef NEBULAKV_CRASH_WRITER_PATH
#error "NEBULAKV_CRASH_WRITER_PATH must be provided by CMake"
#endif

namespace {

using nebulakv::DurabilityMode;
using nebulakv::PersistentKeyValueStore;
using nebulakv::PersistentStoreOptions;
using nebulakv::WalReadIssueCode;
using nebulakv::test::TemporaryDirectory;

PersistentStoreOptions options_for(const std::filesystem::path& path,
                                   const DurabilityMode mode = DurabilityMode::Sync) {
  PersistentStoreOptions options;
  options.wal_path = path;
  options.durability_mode = mode;
  options.batch_flush_interval = std::chrono::milliseconds{10};
  options.emit_recovery_diagnostics = false;
  return options;
}

TEST(PersistentKeyValueStoreTest, DefaultsToSynchronousDurability) {
  TemporaryDirectory directory;
  PersistentStoreOptions options;
  options.wal_path = directory.file("database.wal");
  options.emit_recovery_diagnostics = false;

  PersistentKeyValueStore store{options};

  EXPECT_EQ(store.durability_mode(), DurabilityMode::Sync);
}

TEST(PersistentKeyValueStoreTest, RestoresValuesAfterRestart) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    PersistentKeyValueStore store{options_for(path)};
    store.put("first", "one");
    store.put("second", "two");
  }

  PersistentKeyValueStore recovered{options_for(path)};

  EXPECT_EQ(recovered.get("first"), "one");
  EXPECT_EQ(recovered.get("second"), "two");
  EXPECT_EQ(recovered.size(), 2U);
  EXPECT_EQ(recovered.recovery_report().records_applied, 2U);
}

TEST(PersistentKeyValueStoreTest, RestoresLatestUpdate) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    PersistentKeyValueStore store{options_for(path)};
    store.put("key", "old");
    store.put("key", "new");
  }

  PersistentKeyValueStore recovered{options_for(path)};

  EXPECT_EQ(recovered.get("key"), "new");
  EXPECT_EQ(recovered.size(), 1U);
}

TEST(PersistentKeyValueStoreTest, RestoresDeletion) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    PersistentKeyValueStore store{options_for(path)};
    store.put("key", "value");
    ASSERT_TRUE(store.remove("key"));
  }

  PersistentKeyValueStore recovered{options_for(path)};

  EXPECT_FALSE(recovered.exists("key"));
  EXPECT_EQ(recovered.size(), 0U);
}

TEST(PersistentKeyValueStoreTest, MissingDeleteIsNotWrittenToWal) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  PersistentKeyValueStore store{options_for(path)};
  const auto size_before = std::filesystem::file_size(path);

  EXPECT_FALSE(store.remove("missing"));

  EXPECT_EQ(std::filesystem::file_size(path), size_before);
}

TEST(PersistentKeyValueStoreTest, InvalidPutIsNotWrittenToWal) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  PersistentKeyValueStore store{options_for(path)};
  const auto size_before = std::filesystem::file_size(path);

  EXPECT_THROW(store.put("", "value"), std::invalid_argument);

  EXPECT_EQ(std::filesystem::file_size(path), size_before);
}

TEST(PersistentKeyValueStoreTest, SupportsBatchDurabilityAndExplicitFlush) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    PersistentKeyValueStore store{options_for(path, DurabilityMode::Batch)};
    store.put("key", "value");
    store.flush();
  }

  PersistentKeyValueStore recovered{options_for(path)};
  EXPECT_EQ(recovered.get("key"), "value");
}

TEST(PersistentKeyValueStoreTest, SupportsOperatingSystemBufferedDurability) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    PersistentKeyValueStore store{options_for(path, DurabilityMode::None)};
    store.put("key", "value");
    store.flush();
  }

  PersistentKeyValueStore recovered{options_for(path)};
  EXPECT_EQ(recovered.get("key"), "value");
}

TEST(PersistentKeyValueStoreTest, ConcurrentWritersRecoverInWalOrder) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  constexpr std::size_t kWriterCount = 6;
  constexpr std::size_t kKeysPerWriter = 100;
  {
    PersistentKeyValueStore store{options_for(path, DurabilityMode::None)};
    std::vector<std::thread> writers;
    writers.reserve(kWriterCount);
    for (std::size_t writer = 0; writer < kWriterCount; ++writer) {
      writers.emplace_back([writer, &store] {
        for (std::size_t index = 0; index < kKeysPerWriter; ++index) {
          const std::string key =
              "writer-" + std::to_string(writer) + "-key-" + std::to_string(index);
          store.put(key, "value");
        }
      });
    }
    for (auto& writer : writers) {
      writer.join();
    }
    store.flush();
    EXPECT_EQ(store.size(), kWriterCount * kKeysPerWriter);
  }

  PersistentKeyValueStore recovered{options_for(path)};
  EXPECT_EQ(recovered.size(), kWriterCount * kKeysPerWriter);
}

TEST(PersistentKeyValueStoreTest, RepairsIncompleteTailBeforeAcceptingNewWrites) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    PersistentKeyValueStore store{options_for(path)};
    store.put("first", "one");
    store.put("second", "two");
  }
  std::filesystem::resize_file(path, std::filesystem::file_size(path) - 2U);

  {
    PersistentKeyValueStore repaired{options_for(path)};
    ASSERT_TRUE(repaired.recovery_report().issue.has_value());
    EXPECT_EQ(repaired.recovery_report().issue->code, WalReadIssueCode::IncompleteRecord);
    EXPECT_TRUE(repaired.recovery_report().invalid_tail_truncated);
    EXPECT_EQ(repaired.get("first"), "one");
    EXPECT_FALSE(repaired.exists("second"));
    repaired.put("third", "three");
  }

  PersistentKeyValueStore recovered{options_for(path)};
  EXPECT_EQ(recovered.get("first"), "one");
  EXPECT_EQ(recovered.get("third"), "three");
  EXPECT_FALSE(recovered.exists("second"));
}

TEST(PersistentKeyValueStoreTest, RefusesWritesWhenInvalidTailRepairIsDisabled) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    PersistentKeyValueStore store{options_for(path)};
    store.put("key", "value");
  }
  nebulakv::test::overwrite_byte(path, 16, std::byte{'x'});

  auto options = options_for(path);
  options.truncate_invalid_wal_tail = false;

  EXPECT_THROW(PersistentKeyValueStore store{options}, std::runtime_error);
}

TEST(PersistentKeyValueStoreTest, RestoresAcknowledgedWritesAfterAbruptExit) {
  TemporaryDirectory directory;
  const auto path = directory.file("crash.wal");
  const std::string command =
      std::string{"\""} + NEBULAKV_CRASH_WRITER_PATH + "\" \"" + path.string() + "\"";

  ASSERT_EQ(std::system(command.c_str()), 0);

  PersistentKeyValueStore recovered{options_for(path)};
  ASSERT_EQ(recovered.size(), 1000U);
  for (std::size_t index = 0; index < 1000U; ++index) {
    const std::string key = "key-" + std::to_string(index);
    EXPECT_EQ(recovered.get(key), "value-" + std::to_string(index));
  }
}

TEST(PersistentKeyValueStoreTest, ContinuesSequenceNumbersAfterRecovery) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  {
    PersistentKeyValueStore store{options_for(path)};
    store.put("first", "one");
    store.put("second", "two");
    ASSERT_TRUE(store.remove("first"));
    EXPECT_EQ(store.last_sequence_number(), 3U);
  }

  PersistentKeyValueStore recovered{options_for(path)};
  EXPECT_EQ(recovered.last_sequence_number(), 3U);

  recovered.put("third", "three");

  EXPECT_EQ(recovered.last_sequence_number(), 4U);
  EXPECT_EQ(recovered.get("third"), "three");
}

TEST(PersistentKeyValueStoreTest, AutomaticRotationPersistsSortedMemTables) {
  TemporaryDirectory directory;
  const auto path = directory.file("database.wal");
  auto options = options_for(path);
  options.memtable_max_bytes = 1U;
  {
    PersistentKeyValueStore store{options};
    store.put("charlie", "three");
    store.put("alpha", "one");
    store.put("bravo", "two");
  }

  PersistentKeyValueStore recovered{options};

  EXPECT_EQ(recovered.get("alpha"), "one");
  EXPECT_EQ(recovered.get("bravo"), "two");
  EXPECT_EQ(recovered.get("charlie"), "three");
  EXPECT_EQ(recovered.last_sequence_number(), 3U);
  EXPECT_EQ(recovered.recovery_report().records_applied, 0U);
  EXPECT_EQ(recovered.immutable_memtable_count(), 0U);
  EXPECT_EQ(recovered.sstable_count(), 3U);
  EXPECT_EQ(recovered.active_memtable_memory_usage(), 0U);
}

TEST(PersistentKeyValueStoreTest, CheckpointPersistsStateAndResetsWal) {
  TemporaryDirectory directory;
  const auto path = directory.file("checkpoint.wal");
  auto options = options_for(path);
  options.sstable_data_block_bytes = 128U;
  {
    PersistentKeyValueStore store{options};
    store.put("first", "one");
    store.put("second", "two");
    ASSERT_TRUE(store.remove("first"));

    store.checkpoint();

    EXPECT_EQ(std::filesystem::file_size(path), 0U);
    EXPECT_EQ(store.sstable_count(), 1U);
    EXPECT_EQ(store.immutable_memtable_count(), 0U);
    EXPECT_FALSE(store.exists("first"));
    EXPECT_EQ(store.get("second"), "two");
  }

  PersistentKeyValueStore recovered{options};
  EXPECT_EQ(recovered.recovery_report().records_applied, 0U);
  EXPECT_FALSE(recovered.exists("first"));
  EXPECT_EQ(recovered.get("second"), "two");
  EXPECT_EQ(recovered.size(), 1U);
}

TEST(PersistentKeyValueStoreTest, AutomaticRotationFlushesImmutableTableToDisk) {
  TemporaryDirectory directory;
  const auto path = directory.file("rotation.wal");
  auto options = options_for(path);
  options.memtable_max_bytes = 1U;
  options.sstable_data_block_bytes = 128U;

  PersistentKeyValueStore store{options};
  store.put("key", "value");

  EXPECT_EQ(store.immutable_memtable_count(), 0U);
  EXPECT_EQ(store.sstable_count(), 1U);
  EXPECT_EQ(store.get("key"), "value");
}

TEST(PersistentKeyValueStoreTest, RestartContinuesSequenceAboveSSTables) {
  TemporaryDirectory directory;
  const auto path = directory.file("sequence-sstable.wal");
  auto options = options_for(path);
  {
    PersistentKeyValueStore store{options};
    store.put("first", "one");
    store.put("second", "two");
    store.checkpoint();
    EXPECT_EQ(store.last_sequence_number(), 2U);
  }

  PersistentKeyValueStore recovered{options};
  EXPECT_EQ(recovered.last_sequence_number(), 2U);
  recovered.put("third", "three");
  EXPECT_EQ(recovered.last_sequence_number(), 3U);
}

TEST(PersistentKeyValueStoreTest, WalReplayOverridesOlderSSTableValue) {
  TemporaryDirectory directory;
  const auto path = directory.file("overlay.wal");
  auto options = options_for(path);
  {
    PersistentKeyValueStore store{options};
    store.put("key", "persisted");
    store.checkpoint();
    store.put("key", "wal-newer");
  }

  PersistentKeyValueStore recovered{options};
  EXPECT_EQ(recovered.get("key"), "wal-newer");
  EXPECT_EQ(recovered.size(), 1U);
}

TEST(PersistentKeyValueStoreTest, TombstoneInSSTableHidesOlderValueAfterRestart) {
  TemporaryDirectory directory;
  const auto path = directory.file("tombstone-sstable.wal");
  auto options = options_for(path);
  {
    PersistentKeyValueStore store{options};
    store.put("key", "value");
    store.checkpoint();
    ASSERT_TRUE(store.remove("key"));
    store.checkpoint();
    EXPECT_EQ(store.sstable_count(), 2U);
  }

  PersistentKeyValueStore recovered{options};
  EXPECT_FALSE(recovered.exists("key"));
  EXPECT_EQ(recovered.size(), 0U);
}

TEST(PersistentKeyValueStoreTest, CorruptedSSTableFailsStartupClearly) {
  TemporaryDirectory directory;
  const auto path = directory.file("corrupt-sstable.wal");
  auto options = options_for(path);
  std::filesystem::path table_path;
  {
    PersistentKeyValueStore store{options};
    store.put("key", "value");
    store.checkpoint();
    ASSERT_EQ(store.sstable_metadata().size(), 1U);
    table_path = store.sstable_metadata().front().path;
  }

  const auto size = std::filesystem::file_size(table_path);
  std::filesystem::resize_file(table_path, size - 4U);

  EXPECT_THROW(PersistentKeyValueStore recovered{options}, nebulakv::SSTableCorruptionError);
}

TEST(PersistentKeyValueStoreTest, CheckpointSupportsMultipleDataBlocks) {
  TemporaryDirectory directory;
  const auto path = directory.file("multi-block.wal");
  auto options = options_for(path);
  options.sstable_data_block_bytes = 96U;

  PersistentKeyValueStore store{options};
  for (std::size_t index = 0; index < 50U; ++index) {
    store.put("key-" + std::to_string(1000U + index), "value-" + std::to_string(index));
  }
  store.checkpoint();

  ASSERT_EQ(store.sstable_metadata().size(), 1U);
  EXPECT_GT(store.sstable_metadata().front().block_count, 1U);
  EXPECT_EQ(store.get("key-1025"), "value-25");
}

} // namespace
