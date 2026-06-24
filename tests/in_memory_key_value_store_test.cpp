#include "nebulakv/in_memory_key_value_store.hpp"
#include "nebulakv/key_value_store.hpp"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

using nebulakv::InMemoryKeyValueStore;

TEST(InMemoryKeyValueStoreTest, InsertsNewKey) {
  InMemoryKeyValueStore store;

  store.put("name", "NebulaKV");

  EXPECT_TRUE(store.exists("name"));
  EXPECT_EQ(store.size(), 1U);
}

TEST(InMemoryKeyValueStoreTest, UpdatesExistingKey) {
  InMemoryKeyValueStore store;
  store.put("version", "one");

  store.put("version", "two");

  EXPECT_EQ(store.get("version"), "two");
  EXPECT_EQ(store.size(), 1U);
}

TEST(InMemoryKeyValueStoreTest, RetrievesExistingKey) {
  InMemoryKeyValueStore store;
  store.put("language", "C++20");

  const auto value = store.get("language");

  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "C++20");
}

TEST(InMemoryKeyValueStoreTest, ReturnsNulloptForMissingKey) {
  const InMemoryKeyValueStore store;

  EXPECT_EQ(store.get("missing"), std::nullopt);
}

TEST(InMemoryKeyValueStoreTest, DeletesExistingKey) {
  InMemoryKeyValueStore store;
  store.put("temporary", "value");

  EXPECT_TRUE(store.remove("temporary"));
  EXPECT_FALSE(store.exists("temporary"));
  EXPECT_EQ(store.size(), 0U);
}

TEST(InMemoryKeyValueStoreTest, ReturnsFalseWhenDeletingMissingKey) {
  InMemoryKeyValueStore store;

  EXPECT_FALSE(store.remove("missing"));
}

TEST(InMemoryKeyValueStoreTest, ExistsReturnsTrueForPresentKey) {
  InMemoryKeyValueStore store;
  store.put("present", "yes");

  EXPECT_TRUE(store.exists("present"));
}

TEST(InMemoryKeyValueStoreTest, ExistsReturnsFalseForMissingKey) {
  const InMemoryKeyValueStore store;

  EXPECT_FALSE(store.exists("absent"));
}

TEST(InMemoryKeyValueStoreTest, PutRejectsEmptyKey) {
  InMemoryKeyValueStore store;

  EXPECT_THROW(store.put("", "value"), std::invalid_argument);
}

TEST(InMemoryKeyValueStoreTest, GetRejectsEmptyKey) {
  const InMemoryKeyValueStore store;

  EXPECT_THROW(static_cast<void>(store.get("")), std::invalid_argument);
}

TEST(InMemoryKeyValueStoreTest, RemoveRejectsEmptyKey) {
  InMemoryKeyValueStore store;

  EXPECT_THROW(static_cast<void>(store.remove("")), std::invalid_argument);
}

TEST(InMemoryKeyValueStoreTest, ExistsRejectsEmptyKey) {
  const InMemoryKeyValueStore store;

  EXPECT_THROW(static_cast<void>(store.exists("")), std::invalid_argument);
}

TEST(InMemoryKeyValueStoreTest, AcceptsMaximumLengthKey) {
  InMemoryKeyValueStore store;
  const std::string key(InMemoryKeyValueStore::kMaxKeySize, 'k');

  store.put(key, "value");

  EXPECT_EQ(store.get(key), "value");
}

TEST(InMemoryKeyValueStoreTest, PutRejectsOversizedKey) {
  InMemoryKeyValueStore store;
  const std::string key(InMemoryKeyValueStore::kMaxKeySize + 1U, 'k');

  EXPECT_THROW(store.put(key, "value"), std::length_error);
}

TEST(InMemoryKeyValueStoreTest, ReadOperationsRejectOversizedKey) {
  InMemoryKeyValueStore store;
  const std::string key(InMemoryKeyValueStore::kMaxKeySize + 1U, 'k');

  EXPECT_THROW(static_cast<void>(store.get(key)), std::length_error);
  EXPECT_THROW(static_cast<void>(store.exists(key)), std::length_error);
  EXPECT_THROW(static_cast<void>(store.remove(key)), std::length_error);
}

TEST(InMemoryKeyValueStoreTest, AcceptsMaximumLengthValue) {
  InMemoryKeyValueStore store;
  const std::string value(InMemoryKeyValueStore::kMaxValueSize, 'v');

  store.put("large", value);

  EXPECT_EQ(store.get("large"), value);
}

TEST(InMemoryKeyValueStoreTest, RejectsOversizedValue) {
  InMemoryKeyValueStore store;
  const std::string value(InMemoryKeyValueStore::kMaxValueSize + 1U, 'v');

  EXPECT_THROW(store.put("large", value), std::length_error);
}

TEST(InMemoryKeyValueStoreTest, AcceptsEmptyValue) {
  InMemoryKeyValueStore store;

  store.put("empty-value", "");

  EXPECT_EQ(store.get("empty-value"), "");
}

TEST(InMemoryKeyValueStoreTest, StoresKeysIndependently) {
  InMemoryKeyValueStore store;
  store.put("first", "one");
  store.put("second", "two");

  EXPECT_EQ(store.get("first"), "one");
  EXPECT_EQ(store.get("second"), "two");
  EXPECT_EQ(store.size(), 2U);
}

TEST(InMemoryKeyValueStoreTest, SupportsEmbeddedNullBytes) {
  InMemoryKeyValueStore store;
  const std::string key{"key\0suffix", 10};
  const std::string value{"value\0suffix", 12};

  store.put(key, value);

  EXPECT_EQ(store.get(std::string_view{key.data(), key.size()}), value);
}

TEST(InMemoryKeyValueStoreTest, RemovingOneKeyDoesNotRemoveOthers) {
  InMemoryKeyValueStore store;
  store.put("first", "one");
  store.put("second", "two");

  ASSERT_TRUE(store.remove("first"));

  EXPECT_FALSE(store.exists("first"));
  EXPECT_EQ(store.get("second"), "two");
  EXPECT_EQ(store.size(), 1U);
}

TEST(InMemoryKeyValueStoreTest, WorksThroughKeyValueStoreInterface) {
  const std::unique_ptr<nebulakv::KeyValueStore> store =
      std::make_unique<InMemoryKeyValueStore>();

  store->put("interface", "works");

  EXPECT_EQ(store->get("interface"), "works");
  EXPECT_TRUE(store->exists("interface"));
  EXPECT_TRUE(store->remove("interface"));
}

TEST(InMemoryKeyValueStoreTest, ConcurrentReadersObserveStableValue) {
  InMemoryKeyValueStore store;
  store.put("shared", "stable-value");

  constexpr std::size_t kReaderCount = 8;
  constexpr std::size_t kReadsPerThread = 2'000;
  std::atomic<bool> failed{false};
  std::vector<std::thread> readers;
  readers.reserve(kReaderCount);

  for (std::size_t reader = 0; reader < kReaderCount; ++reader) {
    readers.emplace_back([&store, &failed] {
      for (std::size_t read = 0; read < kReadsPerThread; ++read) {
        if (store.get("shared") != "stable-value") {
          failed.store(true, std::memory_order_relaxed);
          return;
        }
      }
    });
  }

  for (auto& reader : readers) {
    reader.join();
  }

  EXPECT_FALSE(failed.load(std::memory_order_relaxed));
}

TEST(InMemoryKeyValueStoreTest, ConcurrentWritersInsertDistinctKeys) {
  InMemoryKeyValueStore store;
  constexpr std::size_t kWriterCount = 8;
  constexpr std::size_t kKeysPerWriter = 250;
  std::vector<std::thread> writers;
  writers.reserve(kWriterCount);

  for (std::size_t writer = 0; writer < kWriterCount; ++writer) {
    writers.emplace_back([writer, &store] {
      for (std::size_t index = 0; index < kKeysPerWriter; ++index) {
        const auto key = "writer-" + std::to_string(writer) + "-key-" + std::to_string(index);
        store.put(key, "value");
      }
    });
  }

  for (auto& writer : writers) {
    writer.join();
  }

  EXPECT_EQ(store.size(), kWriterCount * kKeysPerWriter);
  for (std::size_t writer = 0; writer < kWriterCount; ++writer) {
    for (std::size_t index = 0; index < kKeysPerWriter; ++index) {
      const auto key = "writer-" + std::to_string(writer) + "-key-" + std::to_string(index);
      EXPECT_TRUE(store.exists(key));
    }
  }
}

TEST(InMemoryKeyValueStoreTest, ConcurrentWritersCanUpdateSameKey) {
  InMemoryKeyValueStore store;
  store.put("shared", "initial");

  constexpr std::size_t kWriterCount = 6;
  constexpr std::size_t kUpdatesPerWriter = 500;
  std::vector<std::thread> writers;
  writers.reserve(kWriterCount);

  for (std::size_t writer = 0; writer < kWriterCount; ++writer) {
    writers.emplace_back([writer, &store] {
      const auto value = "writer-" + std::to_string(writer);
      for (std::size_t update = 0; update < kUpdatesPerWriter; ++update) {
        store.put("shared", value);
      }
    });
  }

  for (auto& writer : writers) {
    writer.join();
  }

  const auto result = store.get("shared");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->find("writer-"), 0U);
  EXPECT_EQ(store.size(), 1U);
}

TEST(InMemoryKeyValueStoreTest, ConcurrentReadersAndWritersRemainConsistent) {
  InMemoryKeyValueStore store;
  store.put("stable", "always-present");

  constexpr std::size_t kWriterCount = 4;
  constexpr std::size_t kReaderCount = 4;
  constexpr std::size_t kOperationsPerThread = 500;
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(kWriterCount + kReaderCount);

  for (std::size_t writer = 0; writer < kWriterCount; ++writer) {
    workers.emplace_back([writer, &store] {
      for (std::size_t index = 0; index < kOperationsPerThread; ++index) {
        const auto key = "mixed-" + std::to_string(writer) + '-' + std::to_string(index);
        store.put(key, "value");
      }
    });
  }

  for (std::size_t reader = 0; reader < kReaderCount; ++reader) {
    workers.emplace_back([&store, &failed] {
      for (std::size_t index = 0; index < kOperationsPerThread; ++index) {
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
  EXPECT_EQ(store.size(), 1U + (kWriterCount * kOperationsPerThread));
}

TEST(InMemoryKeyValueStoreTest, ConcurrentRemovalsDeleteEveryKeyOnce) {
  InMemoryKeyValueStore store;
  constexpr std::size_t kKeyCount = 1'000;
  constexpr std::size_t kRemoverCount = 4;

  for (std::size_t index = 0; index < kKeyCount; ++index) {
    store.put("remove-" + std::to_string(index), "value");
  }

  std::atomic<std::size_t> removed{0};
  std::vector<std::thread> removers;
  removers.reserve(kRemoverCount);

  for (std::size_t remover = 0; remover < kRemoverCount; ++remover) {
    removers.emplace_back([remover, &store, &removed] {
      for (std::size_t index = remover; index < kKeyCount; index += kRemoverCount) {
        if (store.remove("remove-" + std::to_string(index))) {
          removed.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& remover : removers) {
    remover.join();
  }

  EXPECT_EQ(removed.load(std::memory_order_relaxed), kKeyCount);
  EXPECT_EQ(store.size(), 0U);
}

}  // namespace
