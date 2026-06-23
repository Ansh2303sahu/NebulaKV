# NebulaKV

NebulaKV is a modern C++20 key-value storage engine designed around durability, concurrent access,
and an LSM-tree-oriented architecture. It combines a checksummed write-ahead log, crash recovery,
sorted MemTables, immutable SSTables, probabilistic negative lookup acceleration, and a shared
thread-safe block cache.

## Current capabilities

- `KeyValueStore` abstraction with `put`, `get`, `remove`, and `exists`
- Thread-safe hash-based reference store using `std::unordered_map` and `std::shared_mutex`
- Sorted MemTables using `std::map`, monotonic sequence numbers, and tombstone deletion
- Configurable MemTable rotation with active and immutable read coordination
- Checksummed write-ahead log with `sync`, `batch`, and OS-buffered durability modes
- Safe restart recovery, partial-tail handling, corruption detection, and repair controls
- Versioned SSTables with checksummed data blocks, index blocks, headers, and footers
- Indexed point lookups that avoid full-file scans
- Atomic SSTable publication through temporary files, `fsync`, rename, and directory sync
- Explicit checkpoints that persist active memory state before safely resetting the WAL
- Custom Bloom filters with configurable false-positive rate and zero false negatives
- Shared byte-bounded LRU data-block cache using `std::unordered_map`, `std::list`, and `std::mutex`
- Cache hit, miss, eviction, memory, and hit-ratio metrics
- Bloom-filter count, inserted-key, memory, check, and negative-result metrics
- Empty-key validation, 1 KiB key limit, and 1 MiB value limit
- 143 unit and integration tests covering concurrency, recovery, corruption, caching, and filtering
- GCC and Clang builds with warnings treated as errors
- AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer presets
- clang-format, clang-tidy, GoogleTest, Google Benchmark, and GitHub Actions

## Prerequisites

Ubuntu or WSL2:

```bash
sudo apt update
sudo apt install -y build-essential clang clang-tidy clang-format cmake ninja-build git
```

CMake 3.22 or newer, Ninja, Git, and a C++20 compiler are required. The first configure fetches
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

Create a durable checkpoint and exercise SSTable reads:

```bash
./build/debug/nebulakv_cli data/nebulakv.wal --checkpoint
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

SSTable benchmarks separate three read paths:

- Uncached indexed reads that perform file access, block checksum validation, and decoding
- Warm-cache reads that reuse an immutable decoded data block
- Bloom-filter negative reads that avoid block I/O for definitely absent keys

Run the storage benchmarks with repeated aggregate reporting:

```bash
./build/benchmark/benchmarks/nebulakv_benchmarks \
  --benchmark_filter=sstable \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true
```

The benchmark output includes block count, cache hit ratio, cached-block count, Bloom negative count,
and block-read count.

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
├── benchmarks/
│   ├── key_value_store_benchmark.cpp
│   └── sstable_benchmark.cpp
├── cmake/
├── include/nebulakv/
│   ├── block_cache.hpp
│   ├── bloom_filter.hpp
│   ├── entry.hpp
│   ├── key_value_store.hpp
│   ├── memtable.hpp
│   ├── memtable_set.hpp
│   ├── persistent_key_value_store.hpp
│   ├── recovery_manager.hpp
│   ├── sstable_manager.hpp
│   ├── sstable_reader.hpp
│   ├── sstable_writer.hpp
│   ├── wal_reader.hpp
│   ├── wal_record.hpp
│   └── wal_writer.hpp
├── src/
├── tests/
├── CMakeLists.txt
└── CMakePresets.json
```

## Read path

```text
Validate key
      |
Check active MemTable
      |
Check immutable MemTables newest-first
      |
Select candidate SSTables by key range
      |
Bloom filter says definitely absent? ---- yes ---> skip table
      |
      no
      |
Search in-memory block index
      |
Check shared LRU block cache
      |
Cache miss: pread + checksum + decode
      |
Binary-search decoded data block
      |
Return newest sequence or tombstone
```

## Write and checkpoint path

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
Rotate to immutable state at memory threshold
      |
Write checksummed SSTable and publish atomically
      |
Checkpoint resets WAL only after persisted state is synchronized
```

## Correctness guarantees

- Acknowledged synchronous writes are replayed after restart.
- Incomplete final WAL records stop recovery safely.
- WAL and SSTable checksum mismatches report their byte location.
- Tombstones and sequence numbers survive restart.
- Bloom filters never classify an inserted key as definitely absent.
- Cached data blocks are immutable and shared safely between concurrent readers.
- Cache eviction follows least-recently-used ordering under a configurable byte limit.
