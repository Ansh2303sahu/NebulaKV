# NebulaKV

NebulaKV is a modern C++20 key-value storage engine designed around durability, concurrent access,
and an LSM-tree-oriented architecture. The current implementation combines a thread-safe in-memory
API, a checksummed write-ahead log, crash recovery, sorted MemTables, tombstones, and immutable-table
rotation.

## Current capabilities

- `KeyValueStore` abstraction with `put`, `get`, `remove`, and `exists`
- Thread-safe hash-based reference store using `std::unordered_map` and `std::shared_mutex`
- Sorted MemTable storage using `std::map`
- Monotonically increasing sequence numbers
- Tombstone-based deletion
- Configurable MemTable memory threshold, defaulting to 64 MiB
- Atomic active-table rotation with newest-first immutable-table lookup
- Checksummed write-ahead log with `sync`, `batch`, and OS-buffered durability modes
- Safe restart recovery, partial-tail handling, corruption detection, and repair controls
- Empty-key validation, 1 KiB key limit, and 1 MiB value limit
- 99 unit and integration tests, including concurrency, recovery, and corruption scenarios
- GCC and Clang builds with warnings treated as errors
- AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer presets
- clang-format, clang-tidy, GoogleTest, Google Benchmark, and GitHub Actions

## Prerequisites

Ubuntu or WSL2:

```bash
sudo apt update
sudo apt install -y build-essential clang clang-tidy clang-format cmake ninja-build git
```

Required tools are CMake 3.22 or newer, Ninja, Git, and a C++20 compiler. The first configure fetches
pinned GoogleTest or Google Benchmark sources through CMake `FetchContent`.

## Build and test

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Run the storage-engine demonstration:

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

## Release build

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

## Benchmarks

```bash
cmake --preset benchmark
cmake --build --preset benchmark
./build/benchmark/benchmarks/nebulakv_benchmarks
```

The benchmark executable reports three distinct layers:

- Hash-store operations for the reference implementation
- Raw sorted-MemTable operations
- Full `MemTableSet` operations including sequencing and table coordination

`MemTableSet` microbenchmarks disable automatic rotation and publish `immutable_tables` and
`active_bytes` counters. This prevents rotation from being mixed into basic lookup and update
measurements. Rotation and flush behaviour should be measured in dedicated storage-pipeline
benchmarks.

For more stable local results:

```bash
./build/benchmark/benchmarks/nebulakv_benchmarks \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true
```

## Formatting and static analysis

```bash
cmake --build --preset debug --target format
cmake --build --preset debug --target format-check
```

clang-tidy runs as part of configured development builds.

## Repository layout

```text
.
├── .github/workflows/ci.yml
├── app/main.cpp
├── benchmarks/key_value_store_benchmark.cpp
├── cmake/
├── include/nebulakv/
│   ├── entry.hpp
│   ├── in_memory_key_value_store.hpp
│   ├── key_value_store.hpp
│   ├── memtable.hpp
│   ├── memtable_set.hpp
│   ├── persistent_key_value_store.hpp
│   ├── recovery_manager.hpp
│   ├── wal_reader.hpp
│   ├── wal_record.hpp
│   └── wal_writer.hpp
├── src/
├── tests/
├── CMakeLists.txt
└── CMakePresets.json
```

## Storage write path

```text
Validate request
      |
Append checksummed WAL record
      |
Apply configured durability policy
      |
Assign sequence number
      |
Update active sorted MemTable
      |
Rotate to immutable state when the memory threshold is reached
      |
Return success
```

## Recovery guarantees

- Acknowledged synchronous writes are replayed after restart.
- Incomplete final records stop recovery safely.
- Checksum mismatches are reported with their byte offset.
- Valid records before a damaged tail remain recoverable.
- WAL replay restores sequence ordering, updates, deletes, and tombstones.
