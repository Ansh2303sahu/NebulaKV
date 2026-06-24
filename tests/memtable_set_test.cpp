#include "nebulakv/key_value_store.hpp"
#include "nebulakv/memtable_set.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using nebulakv::MemTableOptions;
using nebulakv::MemTableSet;

TEST(MemTableSetTest, ImplementsKeyValueStoreInterface) {
  const std::unique_ptr<nebulakv::KeyValueStore> store =
      std::make_unique<MemTableSet>();

  store->put("key", "value");

  EXPECT_EQ(store->get("key"), "value");
  EXPECT_TRUE(store->exists("key"));
  EXPECT_TRUE(store->remove("key"));
}

TEST(MemTableSetTest, AssignsIncreasingSequenceNumbers) {
  MemTableSet store;

  store.put("first", "one");
  store.put("second", "two");
  store.put("first", "updated");

  EXPECT_EQ(store.latest_entry("first")->sequence_number, 3U);
  EXPECT_EQ(store.latest_entry("second")->sequence_number, 2U);
  EXPECT_EQ(store.last_sequence_number(), 3U);
}

TEST(MemTableSetTest, UpdatesDoNotIncreaseLiveKeyCount) {
  MemTableSet store;
  store.put("key", "one");

  store.put("key", "two");

  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.get("key"), "two");
}

TEST(MemTableSetTest, DeleteCreatesTombstone) {
  MemTableSet store;
  store.put("key", "value");

  ASSERT_TRUE(store.remove("key"));

  const auto entry = store.latest_entry("key");
  ASSERT_TRUE(entry.has_value());
  EXPECT_TRUE(entry->deleted);
  EXPECT_EQ(entry->sequence_number, 2U);
  EXPECT_EQ(store.size(), 0U);
  EXPECT_FALSE(store.exists("key"));
}

TEST(MemTableSetTest, MissingDeleteDoesNotConsumeSequenceNumber) {
  MemTableSet store;

  EXPECT_FALSE(store.remove("missing"));

  EXPECT_EQ(store.last_sequence_number(), 0U);
  EXPECT_EQ(store.size(), 0U);
}

TEST(MemTableSetTest, RejectsZeroMemoryLimit) {
  EXPECT_THROW(MemTableSet(MemTableOptions{0}), std::invalid_argument);
}

TEST(MemTableSetTest, AutomaticallyRotatesAtConfiguredLimit) {
  MemTableSet store{MemTableOptions{1}};

  store.put("key", "value");

  EXPECT_EQ(store.immutable_table_count(), 1U);
  EXPECT_EQ(store.active_entry_count(), 0U);
  EXPECT_EQ(store.active_generation(), 1U);
  EXPECT_EQ(store.get("key"), "value");
}

TEST(MemTableSetTest, ManualRotationFreezesActiveTable) {
  MemTableSet store;
  store.put("key", "value");

  const auto rotated = store.rotate_active();

  ASSERT_TRUE(rotated.has_value());
  EXPECT_TRUE((*rotated)->is_immutable());
  EXPECT_EQ((*rotated)->get_entry("key")->value, "value");
  EXPECT_EQ(store.immutable_table_count(), 1U);
  EXPECT_EQ(store.active_generation(), 1U);
}

TEST(MemTableSetTest, EmptyActiveTableIsNotRotated) {
  MemTableSet store;

  EXPECT_FALSE(store.rotate_active().has_value());
  EXPECT_EQ(store.immutable_table_count(), 0U);
  EXPECT_EQ(store.active_generation(), 0U);
}

TEST(MemTableSetTest, ReadsSearchImmutableTables) {
  MemTableSet store;
  store.put("archived", "value");
  ASSERT_TRUE(store.rotate_active().has_value());

  EXPECT_EQ(store.get("archived"), "value");
  EXPECT_TRUE(store.exists("archived"));
}

TEST(MemTableSetTest, ActiveValueOverridesImmutableValue) {
  MemTableSet store;
  store.put("key", "old");
  ASSERT_TRUE(store.rotate_active().has_value());

  store.put("key", "new");

  EXPECT_EQ(store.get("key"), "new");
  EXPECT_EQ(store.latest_entry("key")->sequence_number, 2U);
}

TEST(MemTableSetTest, ActiveTombstoneHidesImmutableValue) {
  MemTableSet store;
  store.put("key", "value");
  ASSERT_TRUE(store.rotate_active().has_value());

  ASSERT_TRUE(store.remove("key"));

  EXPECT_FALSE(store.get("key").has_value());
  EXPECT_FALSE(store.exists("key"));
  EXPECT_TRUE(store.latest_entry("key")->deleted);
}

TEST(MemTableSetTest, NewestImmutableTableWins) {
  MemTableSet store;
  store.put("key", "first");
  ASSERT_TRUE(store.rotate_active().has_value());
  store.put("key", "second");
  ASSERT_TRUE(store.rotate_active().has_value());

  EXPECT_EQ(store.get("key"), "second");
  ASSERT_EQ(store.immutable_tables().size(), 2U);
  EXPECT_GT(store.immutable_tables()[0]->generation(),
            store.immutable_tables()[1]->generation());
}

TEST(MemTableSetTest, ImmutableSnapshotsRemainSortedAndStable) {
  MemTableSet store;
  store.put("zulu", "last");
  store.put("alpha", "first");
  const auto rotated = store.rotate_active();
  ASSERT_TRUE(rotated.has_value());

  const auto snapshot = (*rotated)->snapshot();
  store.put("middle", "new-active");

  ASSERT_EQ(snapshot.size(), 2U);
  EXPECT_EQ(snapshot[0].first, "alpha");
  EXPECT_EQ(snapshot[1].first, "zulu");
  EXPECT_EQ((*rotated)->entry_count(), 2U);
}

TEST(MemTableSetTest, ReinsertAfterDeleteRestoresLiveKey) {
  MemTableSet store;
  store.put("key", "one");
  ASSERT_TRUE(store.remove("key"));

  store.put("key", "two");

  EXPECT_EQ(store.get("key"), "two");
  EXPECT_EQ(store.size(), 1U);
  EXPECT_EQ(store.last_sequence_number(), 3U);
}

TEST(MemTableSetTest, ConcurrentWritersReceiveUniqueSequences) {
  MemTableSet store{MemTableOptions{2048}};
  constexpr std::size_t kWriterCount = 6;
  constexpr std::size_t kKeysPerWriter = 200;
  std::vector<std::thread> writers;
  writers.reserve(kWriterCount);

  for (std::size_t writer = 0; writer < kWriterCount; ++writer) {
    writers.emplace_back([writer, &store] {
      for (std::size_t index = 0; index < kKeysPerWriter; ++index) {
        store.put("writer-" + std::to_string(writer) + "-key-" + std::to_string(index),
                  "value");
      }
    });
  }
  for (auto& writer : writers) {
    writer.join();
  }

  EXPECT_EQ(store.size(), kWriterCount * kKeysPerWriter);
  EXPECT_EQ(store.last_sequence_number(), kWriterCount * kKeysPerWriter);
  EXPECT_GT(store.immutable_table_count(), 0U);
}

TEST(MemTableSetTest, ConcurrentReadersRemainCorrectDuringRotation) {
  MemTableSet store{MemTableOptions{512}};
  store.put("stable", "always-present");

  constexpr std::size_t kWriterCount = 4;
  constexpr std::size_t kReaderCount = 4;
  constexpr std::size_t kOperations = 500;
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(kWriterCount + kReaderCount);

  for (std::size_t writer = 0; writer < kWriterCount; ++writer) {
    workers.emplace_back([writer, &store] {
      for (std::size_t index = 0; index < kOperations; ++index) {
        store.put("dynamic-" + std::to_string(writer) + '-' + std::to_string(index),
                  "value");
      }
    });
  }
  for (std::size_t reader = 0; reader < kReaderCount; ++reader) {
    workers.emplace_back([&store, &failed] {
      for (std::size_t index = 0; index < kOperations; ++index) {
        if (store.get("stable") != "always-present" || !store.exists("stable")) {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }

  EXPECT_FALSE(failed.load(std::memory_order_relaxed));
  EXPECT_EQ(store.size(), 1U + (kWriterCount * kOperations));
}

TEST(MemTableSetTest, ConcurrentUpdatesAndDeletesRemainConsistent) {
  MemTableSet store{MemTableOptions{1024}};
  constexpr std::size_t kKeyCount = 400;
  for (std::size_t index = 0; index < kKeyCount; ++index) {
    store.put("key-" + std::to_string(index), "initial");
  }

  std::thread updater{[&store] {
    for (std::size_t index = 0; index < kKeyCount; index += 2U) {
      store.put("key-" + std::to_string(index), "updated");
    }
  }};
  std::thread remover{[&store] {
    for (std::size_t index = 1; index < kKeyCount; index += 2U) {
      static_cast<void>(store.remove("key-" + std::to_string(index)));
    }
  }};

  updater.join();
  remover.join();

  EXPECT_EQ(store.size(), kKeyCount / 2U);
  for (std::size_t index = 0; index < kKeyCount; ++index) {
    const auto key = "key-" + std::to_string(index);
    if (index % 2U == 0U) {
      EXPECT_EQ(store.get(key), "updated");
    } else {
      EXPECT_FALSE(store.exists(key));
      ASSERT_TRUE(store.latest_entry(key).has_value());
      EXPECT_TRUE(store.latest_entry(key)->deleted);
    }
  }
}

}  // namespace
