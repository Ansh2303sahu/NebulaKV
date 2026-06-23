#!/usr/bin/env bash
set -euo pipefail

if [[ ! -f CMakeLists.txt || ! -d .git ]]; then
  echo "Run this script from the root of your NebulaKV Git repository." >&2
  exit 1
fi

mkdir -p app benchmarks include/nebulakv src tests
rm -rf include/industry_starter
rm -f src/core.cpp tests/core_test.cpp benchmarks/core_benchmark.cpp

cat > CMakeLists.txt <<'NEBULAKV_PHASE2_CMAKELISTS_TXT'
cmake_minimum_required(VERSION 3.22)

project(
  NebulaKV
  VERSION 0.2.0
  DESCRIPTION "Distributed key-value database built in modern C++"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_CLANG_TIDY "Run clang-tidy while compiling project targets" OFF)
option(ENABLE_BENCHMARKS "Build Google Benchmark targets" OFF)
option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)
include(Formatting)

find_package(Threads REQUIRED)

add_library(nebulakv_core src/in_memory_key_value_store.cpp)
add_library(NebulaKV::core ALIAS nebulakv_core)

target_include_directories(
  nebulakv_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_compile_features(nebulakv_core PUBLIC cxx_std_20)
target_link_libraries(nebulakv_core PUBLIC Threads::Threads)
configure_project_target(nebulakv_core)

add_executable(nebulakv_cli app/main.cpp)
target_link_libraries(nebulakv_cli PRIVATE NebulaKV::core)
configure_project_target(nebulakv_cli)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(ENABLE_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
NEBULAKV_PHASE2_CMAKELISTS_TXT

cat > README.md <<'NEBULAKV_PHASE2_README_MD'
# NebulaKV

NebulaKV is a distributed key-value database being built in modern C++20. Phase 2 provides a
single-node, thread-safe, in-memory storage engine backed by `std::unordered_map` and protected
with `std::shared_mutex`.

## Current features

- `KeyValueStore` interface with `put`, `get`, `remove`, and `exists`
- Thread-safe `InMemoryKeyValueStore`
- Shared locks for reads and exclusive locks for writes
- Heterogeneous `std::string_view` lookup without temporary key allocation
- Empty-key validation
- Maximum key size: 1 KiB
- Maximum value size: 1 MiB
- 27 unit tests, including concurrent readers, writers, mixed access, and removals
- GCC and Clang builds
- AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer presets
- clang-format, clang-tidy, GoogleTest, Google Benchmark, and GitHub Actions

## Prerequisites

Ubuntu or WSL2:

```bash
sudo apt update
sudo apt install -y build-essential clang clang-tidy clang-format cmake ninja-build git
```

Required: CMake 3.22+, a C++20 compiler, Ninja, and Git. The first configure downloads pinned
GoogleTest or Google Benchmark sources with CMake FetchContent.

## Build and test

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Run the demonstration program:

```bash
./build/debug/nebulakv_cli
```

## Sanitizers

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan
```

## Release and benchmarks

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release

cmake --preset benchmark
cmake --build --preset benchmark
./build/benchmark/benchmarks/nebulakv_benchmarks
```

## Formatting

```bash
cmake --build --preset debug --target format
cmake --build --preset debug --target format-check
```

## Repository layout

```text
.
├── .github/workflows/ci.yml
├── app/main.cpp
├── benchmarks/key_value_store_benchmark.cpp
├── cmake/
├── include/nebulakv/
│   ├── in_memory_key_value_store.hpp
│   └── key_value_store.hpp
├── src/in_memory_key_value_store.cpp
├── tests/in_memory_key_value_store_test.cpp
├── CMakeLists.txt
└── CMakePresets.json
```

## Phase 2 completion checklist

- [x] In-memory `std::unordered_map` storage
- [x] `std::shared_mutex` synchronization
- [x] Shared read locks and exclusive write locks
- [x] At least 20 unit tests
- [x] Empty key and size-limit validation
- [x] Concurrent reader and writer tests
- [ ] Confirm ASan/UBSan is green on the local machine and GitHub Actions
- [ ] Confirm TSan is green on the local machine and GitHub Actions
NEBULAKV_PHASE2_README_MD

cat > app/main.cpp <<'NEBULAKV_PHASE2_APP_MAIN_CPP'
#include "nebulakv/in_memory_key_value_store.hpp"

#include <iostream>
#include <string>

int main() {
  nebulakv::InMemoryKeyValueStore store;
  store.put("project", "NebulaKV");
  store.put("phase", "in-memory engine");

  const auto project = store.get("project");
  const auto phase = store.get("phase");

  if (!project || !phase) {
    std::cerr << "Failed to read the demonstration keys\n";
    return 1;
  }

  std::cout << *project << ": " << *phase << '\n';
  std::cout << "entries=" << store.size() << '\n';
  return 0;
}
NEBULAKV_PHASE2_APP_MAIN_CPP

cat > benchmarks/CMakeLists.txt <<'NEBULAKV_PHASE2_BENCHMARKS_CMAKELISTS_TXT'
include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(nebulakv_benchmarks key_value_store_benchmark.cpp)
target_link_libraries(nebulakv_benchmarks PRIVATE NebulaKV::core benchmark::benchmark_main)
configure_project_target(nebulakv_benchmarks)
NEBULAKV_PHASE2_BENCHMARKS_CMAKELISTS_TXT

cat > benchmarks/key_value_store_benchmark.cpp <<'NEBULAKV_PHASE2_BENCHMARKS_KEY_VALUE_STORE_BENCHMARK_CPP'
#include "nebulakv/in_memory_key_value_store.hpp"

#include <cstddef>
#include <string>

#include <benchmark/benchmark.h>

namespace {

void benchmark_get(benchmark::State& state) {
  nebulakv::InMemoryKeyValueStore store;
  store.put("benchmark-key", std::string(static_cast<std::size_t>(state.range(0)), 'v'));

  for (auto _ : state) {
    (void)_;
    const auto value = store.get("benchmark-key");
    benchmark::DoNotOptimize(value);
  }
}

void benchmark_put_update(benchmark::State& state) {
  nebulakv::InMemoryKeyValueStore store;
  const std::string value(static_cast<std::size_t>(state.range(0)), 'v');

  for (auto _ : state) {
    (void)_;
    store.put("benchmark-key", value);
    benchmark::ClobberMemory();
  }
}

}  // namespace

BENCHMARK(benchmark_get)->RangeMultiplier(8)->Range(8, 8 << 15);
BENCHMARK(benchmark_put_update)->RangeMultiplier(8)->Range(8, 8 << 15);
NEBULAKV_PHASE2_BENCHMARKS_KEY_VALUE_STORE_BENCHMARK_CPP

cat > include/nebulakv/key_value_store.hpp <<'NEBULAKV_PHASE2_INCLUDE_NEBULAKV_KEY_VALUE_STORE_HPP'
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace nebulakv {

class KeyValueStore {
 public:
  virtual ~KeyValueStore() = default;

  virtual void put(std::string key, std::string value) = 0;

  [[nodiscard]] virtual std::optional<std::string> get(std::string_view key) const = 0;

  virtual bool remove(std::string_view key) = 0;

  [[nodiscard]] virtual bool exists(std::string_view key) const = 0;
};

}  // namespace nebulakv
NEBULAKV_PHASE2_INCLUDE_NEBULAKV_KEY_VALUE_STORE_HPP

cat > include/nebulakv/in_memory_key_value_store.hpp <<'NEBULAKV_PHASE2_INCLUDE_NEBULAKV_IN_MEMORY_KEY_VALUE_STORE_HPP'
#pragma once

#include "nebulakv/key_value_store.hpp"

#include <cstddef>
#include <functional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace nebulakv {

class InMemoryKeyValueStore final : public KeyValueStore {
 public:
  static constexpr std::size_t kMaxKeySize = 1024;
  static constexpr std::size_t kMaxValueSize = 1024 * 1024;

  InMemoryKeyValueStore() = default;
  ~InMemoryKeyValueStore() override = default;

  InMemoryKeyValueStore(const InMemoryKeyValueStore&) = delete;
  InMemoryKeyValueStore& operator=(const InMemoryKeyValueStore&) = delete;
  InMemoryKeyValueStore(InMemoryKeyValueStore&&) = delete;
  InMemoryKeyValueStore& operator=(InMemoryKeyValueStore&&) = delete;

  void put(std::string key, std::string value) override;

  [[nodiscard]] std::optional<std::string> get(std::string_view key) const override;

  bool remove(std::string_view key) override;

  [[nodiscard]] bool exists(std::string_view key) const override;

  [[nodiscard]] std::size_t size() const;

 private:
  struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept;
    [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept;
  };

  using Storage =
      std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>>;

  static void validate_key(std::string_view key);
  static void validate_value(std::string_view value);

  mutable std::shared_mutex mutex_;
  Storage entries_;
};

}  // namespace nebulakv
NEBULAKV_PHASE2_INCLUDE_NEBULAKV_IN_MEMORY_KEY_VALUE_STORE_HPP

cat > src/in_memory_key_value_store.cpp <<'NEBULAKV_PHASE2_SRC_IN_MEMORY_KEY_VALUE_STORE_CPP'
#include "nebulakv/in_memory_key_value_store.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace nebulakv {

std::size_t InMemoryKeyValueStore::TransparentStringHash::operator()(
    const std::string_view value) const noexcept {
  return std::hash<std::string_view>{}(value);
}

std::size_t InMemoryKeyValueStore::TransparentStringHash::operator()(
    const std::string& value) const noexcept {
  return (*this)(std::string_view{value});
}

void InMemoryKeyValueStore::put(std::string key, std::string value) {
  validate_key(key);
  validate_value(value);

  std::unique_lock lock{mutex_};
  entries_.insert_or_assign(std::move(key), std::move(value));
}

std::optional<std::string> InMemoryKeyValueStore::get(const std::string_view key) const {
  validate_key(key);

  std::shared_lock lock{mutex_};
  const auto entry = entries_.find(key);
  if (entry == entries_.end()) {
    return std::nullopt;
  }

  return entry->second;
}

bool InMemoryKeyValueStore::remove(const std::string_view key) {
  validate_key(key);

  std::unique_lock lock{mutex_};
  const auto entry = entries_.find(key);
  if (entry == entries_.end()) {
    return false;
  }

  entries_.erase(entry);
  return true;
}

bool InMemoryKeyValueStore::exists(const std::string_view key) const {
  validate_key(key);

  std::shared_lock lock{mutex_};
  return entries_.contains(key);
}

std::size_t InMemoryKeyValueStore::size() const {
  std::shared_lock lock{mutex_};
  return entries_.size();
}

void InMemoryKeyValueStore::validate_key(const std::string_view key) {
  if (key.empty()) {
    throw std::invalid_argument{"NebulaKV keys must not be empty"};
  }

  if (key.size() > kMaxKeySize) {
    throw std::length_error{"NebulaKV key exceeds the maximum size"};
  }
}

void InMemoryKeyValueStore::validate_value(const std::string_view value) {
  if (value.size() > kMaxValueSize) {
    throw std::length_error{"NebulaKV value exceeds the maximum size"};
  }
}

}  // namespace nebulakv
NEBULAKV_PHASE2_SRC_IN_MEMORY_KEY_VALUE_STORE_CPP

cat > tests/CMakeLists.txt <<'NEBULAKV_PHASE2_TESTS_CMAKELISTS_TXT'
include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(nebulakv_tests in_memory_key_value_store_test.cpp)
target_link_libraries(nebulakv_tests PRIVATE NebulaKV::core GTest::gtest_main)
configure_project_target(nebulakv_tests)

include(GoogleTest)
gtest_discover_tests(nebulakv_tests DISCOVERY_TIMEOUT 30)
NEBULAKV_PHASE2_TESTS_CMAKELISTS_TXT

cat > tests/in_memory_key_value_store_test.cpp <<'NEBULAKV_PHASE2_TESTS_IN_MEMORY_KEY_VALUE_STORE_TEST_CPP'
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
NEBULAKV_PHASE2_TESTS_IN_MEMORY_KEY_VALUE_STORE_TEST_CPP

echo "Phase 2 files installed."
echo "Next: rm -rf build && cmake --preset debug && cmake --build --preset debug && ctest --preset debug"
