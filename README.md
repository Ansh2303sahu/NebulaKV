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
