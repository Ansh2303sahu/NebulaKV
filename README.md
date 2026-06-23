# NebulaKV

NebulaKV is a durable key-value database written in modern C++20. The current storage engine
combines a thread-safe in-memory index with a checksummed write-ahead log so acknowledged writes
can be recovered after a normal restart or abrupt process termination.

## Storage guarantees

Every persistent write follows this order:

```text
Validate request
      |
Append checksummed WAL record
      |
Apply configured durability policy
      |
Update the in-memory index
      |
Return success
```

A write is never exposed in memory before its log record has been appended. Persistent writes are
serialized across the WAL and memory update, preserving the same operation order during live
execution and recovery.

## Current capabilities

- `KeyValueStore` abstraction with `put`, `get`, `remove`, and `exists`
- Thread-safe `InMemoryKeyValueStore` backed by `std::unordered_map`
- `PersistentKeyValueStore` with automatic startup recovery
- Shared locks for reads and exclusive locks for in-memory writes
- Serialized persistent write path for deterministic WAL ordering
- Binary WAL records with magic bytes, format version, operation, lengths, payload, and CRC-32
- `Put` and `Delete` operation replay
- Synchronous, periodic batch, and operating-system-buffered durability modes
- Safe handling of incomplete records, malformed lengths, unsupported operations, and corruption
- Byte-offset diagnostics for damaged records
- Optional truncation of an invalid WAL tail before accepting new writes
- Empty-key validation, 1 KiB key limit, and 1 MiB value limit
- Automated restart, abrupt-exit, corruption, concurrency, sanitizer, and benchmark coverage
- GCC and Clang builds with warnings treated as errors
- clang-format, clang-tidy, GoogleTest, Google Benchmark, and GitHub Actions

## WAL record format

```text
+----------+---------+-----------+----------+------------+-----+-------+----------+
| Magic(4) | Ver.(2) | Op.(1)    | Flags(1) | Key Len(4) | Key | Value | CRC32(4) |
|          |         |           |          | Val Len(4) |     |       |          |
+----------+---------+-----------+----------+------------+-----+-------+----------+
```

All integer fields use little-endian encoding. The CRC-32 covers the complete header and payload,
excluding the checksum field itself.

## Durability modes

| Mode | Behaviour | Intended use |
|---|---|---|
| `sync` | Calls `fsync` after every appended record | Default; strongest acknowledged-write durability |
| `batch` | Flushes dirty WAL data on a configurable interval | Higher throughput with a bounded durability window |
| `none` | Relies on operating-system buffering | Development, benchmarks, or explicitly accepted risk |

Calling `flush()` forces an `fsync` regardless of the configured mode.

## Prerequisites

Ubuntu or WSL2:

```bash
sudo apt update
sudo apt install -y build-essential clang clang-tidy clang-format cmake ninja-build git
```

Required: CMake 3.22+, a C++20 compiler, Ninja, and Git. The first configure downloads pinned
GoogleTest or Google Benchmark sources through CMake `FetchContent`.

## Build and test

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Run the durable command-line demonstration:

```bash
./build/debug/nebulakv_cli
```

Provide a custom WAL path:

```bash
./build/debug/nebulakv_cli /tmp/nebulakv-demo/database.wal
```

Run it twice to observe startup replay through the `recovered_records` field.

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

## Formatting and static analysis

```bash
cmake --build --preset debug --target format
cmake --build --preset debug --target format-check
```

`clang-tidy` runs automatically on project targets in the debug preset.

## Repository layout

```text
.
├── .github/workflows/ci.yml
├── app/main.cpp
├── benchmarks/key_value_store_benchmark.cpp
├── cmake/
├── include/nebulakv/
│   ├── checksum_calculator.hpp
│   ├── durability_mode.hpp
│   ├── in_memory_key_value_store.hpp
│   ├── key_value_store.hpp
│   ├── persistent_key_value_store.hpp
│   ├── recovery_manager.hpp
│   ├── storage_limits.hpp
│   ├── validation.hpp
│   ├── wal_reader.hpp
│   ├── wal_record.hpp
│   └── wal_writer.hpp
├── src/
├── tests/
├── CMakeLists.txt
└── CMakePresets.json
```

## Recovery behaviour

Recovery scans records sequentially and applies only fully validated operations. On an incomplete or
corrupted record, it:

1. Stops at the first unsafe byte offset.
2. Retains every previously validated operation.
3. Reports a structured diagnostic containing the failure type and byte offset.
4. Optionally truncates the invalid tail so new writes remain recoverable.

The automated abrupt-exit test writes 1,000 synchronously durable keys in a child process, exits
without running destructors, reopens the database, and verifies every acknowledged value.
