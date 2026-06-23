# NebulaKV

NebulaKV is a modern C++20 key-value storage engine built around an LSM-tree architecture. It
combines concurrent in-memory access, checksummed write-ahead logging, sorted MemTables, immutable
rotation, persistent SSTables, indexed point reads, tombstones, and restart recovery.

## Current capabilities

- `KeyValueStore` abstraction with `put`, `get`, `remove`, and `exists`
- Thread-safe hash-based reference store using `std::unordered_map` and `std::shared_mutex`
- Sorted MemTables using `std::map`
- Monotonically increasing 64-bit sequence numbers
- Tombstone-based deletion
- Configurable MemTable memory threshold, defaulting to 64 MiB
- Atomic active-to-immutable MemTable rotation
- Checksummed write-ahead log with `sync`, `batch`, and OS-buffered durability modes
- Safe WAL recovery with incomplete-tail handling and byte-offset diagnostics
- Immutable sorted-string-table files with versioned headers and footers
- Target-sized data blocks with independent CRC-32 checksums
- A checksummed index containing block offsets and key ranges
- Binary search inside the selected data block
- Atomic SSTable publication through temporary file, `fsync`, rename, and directory `fsync`
- Automatic immutable-table flushing and explicit storage checkpoints
- Restart loading from SSTables with sequence continuation
- Tombstone and newest-sequence resolution across multiple SSTables
- 1 KiB key limit and 1 MiB value limit
- 125 unit and integration tests
- GCC and Clang warnings-as-errors builds
- AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer presets
- clang-format, clang-tidy, GoogleTest, Google Benchmark, and GitHub Actions

## Storage architecture

```text
Client write
    |
Validate key and value
    |
Append checksummed WAL record
    |
Apply durability policy
    |
Assign sequence number
    |
Update active sorted MemTable
    |
Rotate when the configured memory threshold is reached
    |
Write immutable MemTable to a temporary SSTable
    |
fsync file -> atomic rename -> fsync directory
    |
Publish the SSTable reader and release the immutable MemTable
```

Point reads use the following order:

```text
Active MemTable
    |
Immutable MemTables, newest first
    |
SSTables
    |
Compare matching sequence numbers and preserve tombstones
```

## SSTable file layout

```text
+-------------------------------+
| Versioned header + CRC-32     |
+-------------------------------+
| Data block 0 + CRC-32         |
+-------------------------------+
| Data block 1 + CRC-32         |
+-------------------------------+
| ...                           |
+-------------------------------+
| Key-range index + CRC-32      |
+-------------------------------+
| Footer with offsets + CRC-32  |
+-------------------------------+
```

Each data record stores:

```text
key length | value length | sequence number | tombstone flag | key | value
```

The index stores the first and last key, byte offset, and encoded size of every data block. A point
lookup binary-searches the index, reads only the candidate block, validates its checksum, and then
binary-searches the records in that block.

## Checkpoints and WAL lifecycle

`checkpoint()` performs the following while serializing writes:

1. Flush the WAL according to the configured durability policy.
2. Freeze the active MemTable.
3. Persist every immutable MemTable as an SSTable.
4. Publish the new SSTable readers.
5. Reset the WAL only after all current state is durable on disk.

Automatic MemTable rotation uses the same durable publication path. When a rotation leaves no
unpersisted active entries, NebulaKV safely resets the covered WAL prefix by truncating the file.

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

Create a durable checkpoint during the demonstration:

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

The benchmark executable reports:

- Hash-store reads and updates
- Raw sorted-MemTable reads and updates
- Full `MemTableSet` reads and updates
- Indexed SSTable point reads at 1,000, 10,000, and 100,000 keys

For stable local measurements:

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
├── benchmarks/
├── cmake/
├── include/nebulakv/
│   ├── data_block.hpp
│   ├── entry.hpp
│   ├── index_block.hpp
│   ├── memtable.hpp
│   ├── memtable_set.hpp
│   ├── persistent_key_value_store.hpp
│   ├── recovery_manager.hpp
│   ├── sstable_error.hpp
│   ├── sstable_footer.hpp
│   ├── sstable_manager.hpp
│   ├── sstable_metadata.hpp
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

## Corruption behaviour

- Header, data-block, index, and footer checksums are validated independently.
- Invalid offsets and lengths are rejected before allocating record buffers.
- Truncated files and malformed metadata produce `SSTableCorruptionError`.
- Corruption errors include the byte offset associated with the failed structure.
- A corrupted SSTable prevents startup rather than silently returning stale or incomplete data.
- Abandoned `.sst.tmp` files are removed during startup discovery.

Startup currently discovers validated `.sst` files from the configured storage directory. A durable
manifest and atomic table-set metadata update are introduced with the compaction subsystem.
