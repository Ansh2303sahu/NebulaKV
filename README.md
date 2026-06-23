# NebulaKV

NebulaKV is a modern C++20 key-value storage engine built around durability, concurrent access,
and an LSM-tree architecture. It combines a checksummed write-ahead log, crash recovery, sorted
MemTables, indexed SSTables, Bloom-filter negative lookups, a shared LRU block cache, atomic
manifest management, and leveled compaction.

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
- Level 0 and Level 1 SSTable organization
- Atomic `MANIFEST-*` and `CURRENT` metadata publication
- Overlap-aware L0-to-L1 compaction
- Newest-sequence resolution and safe tombstone retirement
- Orphan-file cleanup after interrupted publication
- Open-file reader snapshots that remain valid while obsolete SSTables are unlinked
- Cache, Bloom-filter, and compaction statistics
- Empty-key validation, 1 KiB key limit, and 1 MiB value limit
- 162 unit and integration tests covering concurrency, recovery, corruption, caching, manifests,
  and compaction
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

Create a durable checkpoint:

```bash
./build/debug/nebulakv_cli data/nebulakv.wal --checkpoint
```

Force an L0-to-L1 compaction:

```bash
./build/debug/nebulakv_cli data/nebulakv.wal --checkpoint --compact
```

The CLI reports the active manifest, level counts, cache statistics, Bloom-filter memory, and
compaction counters.

## Sanitizers

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --output-on-failure
```

## Release build

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

## Benchmarks

```bash
cmake --preset benchmark
cmake --build --preset benchmark
./build/benchmark/benchmarks/nebulakv_benchmarks
```

SSTable benchmarks separate:

- Uncached indexed reads that perform file access, checksum validation, and decoding
- Warm-cache reads that reuse immutable decoded blocks
- Bloom-filter negative reads that avoid block I/O for definitely absent keys

Run the read benchmarks with aggregate reporting:

```bash
./build/benchmark/benchmarks/nebulakv_benchmarks \
  --benchmark_filter=sstable \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true
```

Run compaction benchmarks:

```bash
./build/benchmark/benchmarks/nebulakv_benchmarks \
  --benchmark_filter=compaction \
  --benchmark_repetitions=3 \
  --benchmark_report_aggregates_only=true
```

Compaction output reports input-table count, input-entry count, output-entry count, and processed
entries per second.

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
│   ├── compaction_benchmark.cpp
│   ├── key_value_store_benchmark.cpp
│   └── sstable_benchmark.cpp
├── cmake/
├── include/nebulakv/
│   ├── block_cache.hpp
│   ├── bloom_filter.hpp
│   ├── entry.hpp
│   ├── key_value_store.hpp
│   ├── manifest.hpp
│   ├── memtable.hpp
│   ├── memtable_set.hpp
│   ├── persistent_key_value_store.hpp
│   ├── recovery_manager.hpp
│   ├── sstable_level.hpp
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
Write checksummed L0 SSTable and publish a new manifest
      |
Checkpoint resets WAL only after persisted state is synchronized
```

## Compaction path

```text
Select oldest L0 tables
      |
Expand selection with overlapping L1 tables
      |
Read and merge outside the manager write lock
      |
Keep the highest sequence for every key
      |
Retain tombstones that still protect older values
      |
Write and fsync a new L1 SSTable
      |
Write and fsync a new MANIFEST file
      |
Atomically replace CURRENT
      |
Swap the in-memory reader set
      |
Unlink obsolete SSTables
```

Existing reads retain shared `SSTableReader` instances with open file descriptors. On POSIX
systems, those readers can finish safely after an obsolete file is unlinked. New reads observe the
new manifest-backed reader set.

## Manifest protocol

The SSTable directory contains:

```text
CURRENT
MANIFEST-00000000000000000001
MANIFEST-00000000000000000002
sstable-L0-....sst
sstable-L1-....sst
```

`CURRENT` names the authoritative versioned manifest. Each manifest stores:

- Manifest generation
- Next unique SSTable file identifier
- Active SSTable filenames
- L0 or L1 assignment
- Sequence range
- Key range
- Entry and block counts
- CRC-32 checksum

Publication order is deliberately crash-safe:

1. Write, checksum, and synchronize the new SSTable.
2. Write and synchronize a new versioned manifest.
3. Atomically replace and synchronize `CURRENT`.
4. Publish the new reader snapshot.
5. Delete obsolete files.

A crash before step 3 leaves the previous manifest authoritative. A crash after step 3 leaves the
new manifest authoritative. Unreferenced SSTables are removed during startup only after the active
manifest has loaded successfully.

## Compaction configuration

```cpp
nebulakv::PersistentStoreOptions options;
options.level0_compaction_trigger = 4;
options.level0_compaction_max_tables = 4;
options.enable_automatic_compaction = true;
```

Automatic compaction begins when the L0 table count reaches the configured trigger. Manual
compaction is available through `PersistentKeyValueStore::compact()`.

## Correctness guarantees

- Acknowledged synchronous writes are replayed after restart.
- Incomplete final WAL records stop recovery safely.
- WAL and SSTable checksum mismatches report their byte location.
- Tombstones and sequence numbers survive restart.
- Bloom filters never classify an inserted key as definitely absent.
- Cached data blocks are immutable and shared safely between concurrent readers.
- Cache eviction follows least-recently-used ordering under a configurable byte limit.
- The database opens active SSTables from `CURRENT` and its manifest rather than trusting a
  directory scan.
- L1 key ranges do not overlap.
- Compaction keeps the newest sequence for every key.
- Tombstones are removed only when no unselected older value can become visible.
- A manifest switch occurs before obsolete SSTables are removed.
- Interrupted publication leaves either the previous or the new complete table set recoverable.
