# NebulaKV

NebulaKV is a C++20 key-value database with a durable LSM-tree storage engine and a concurrent
gRPC API. It combines a checksummed write-ahead log, crash recovery, sorted MemTables, indexed
SSTables, Bloom-filter negative lookups, an LRU block cache, atomic manifests, leveled compaction,
and a bounded network execution layer.

## Capabilities

- Protocol Buffers API for `Put`, `Get`, `Delete`, `BatchPut`, and `Status`
- `nebulakv-server` and `nebulakv-cli` executables
- Fixed worker pool with bounded request queue and overload backpressure
- Per-request deadlines and gRPC status-code mapping
- Configurable maximum transport and application message sizes
- Graceful shutdown that drains accepted work and flushes acknowledged writes
- Concurrent remote clients
- Structured status responses with storage, cache, compaction, queue, and request metrics
- Thread-safe in-memory reference store using `std::unordered_map` and `std::shared_mutex`
- Sorted MemTables using `std::map`, monotonic sequence numbers, and tombstones
- Checksummed WAL with `sync`, `batch`, and OS-buffered durability modes
- Restart recovery with incomplete-tail and corruption handling
- Versioned SSTables with checksummed data blocks, indexes, headers, and footers
- Custom Bloom filters and a shared byte-bounded LRU block cache
- Atomic `MANIFEST-*` and `CURRENT` metadata publication
- Overlap-aware L0-to-L1 compaction with safe tombstone retirement
- GCC and Clang builds with warnings treated as errors
- GoogleTest, Google Benchmark, sanitizers, clang-format, clang-tidy, and GitHub Actions

## Prerequisites

Storage-only builds:

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  clang \
  clang-tidy \
  clang-format \
  cmake \
  ninja-build \
  git
```

Network builds also require Protocol Buffers and gRPC development packages:

```bash
sudo apt install -y \
  protobuf-compiler \
  protobuf-compiler-grpc \
  libprotobuf-dev \
  libgrpc++-dev
```

CMake 3.22 or newer, Ninja, Git, and a C++20 compiler are required.

## Storage build

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Run the local storage administration tool:

```bash
./build/debug/nebulakv-storage
```

Create a checkpoint and run compaction:

```bash
./build/debug/nebulakv-storage data/nebulakv.wal --checkpoint --compact
```

## Network build

```bash
cmake --preset network-release
cmake --build --preset network-release
ctest --preset network-release --output-on-failure
```

This build generates C++ Protocol Buffers and gRPC sources from:

```text
proto/nebulakv/v1/key_value_service.proto
```

## Start the server

```bash
./build/network-release/nebulakv-server \
  --host 0.0.0.0 \
  --port 5001 \
  --data-dir data/server \
  --workers 8 \
  --queue-capacity 1024 \
  --durability sync
```

Useful server options:

```text
--max-message-bytes <bytes>
--max-batch-entries <count>
--max-batch-bytes <bytes>
--checkpoint-on-shutdown
```

Send `SIGINT` or `SIGTERM` for graceful shutdown. The server stops accepting new RPCs, drains
accepted work, and flushes the WAL before exiting. With `--checkpoint-on-shutdown`, active memory
state is also persisted to SSTables before the process exits.

## Remote CLI

```bash
./build/network-release/nebulakv-cli --host 127.0.0.1 --port 5001 put user:1 Ansh
./build/network-release/nebulakv-cli --host 127.0.0.1 --port 5001 get user:1
./build/network-release/nebulakv-cli --host 127.0.0.1 --port 5001 delete user:1
./build/network-release/nebulakv-cli --host 127.0.0.1 --port 5001 \
  batch-put user:1 Ansh user:2 Sumeet
./build/network-release/nebulakv-cli --host 127.0.0.1 --port 5001 status
```

Set a client deadline with:

```bash
./build/network-release/nebulakv-cli \
  --timeout-ms 500 \
  --host 127.0.0.1 \
  --port 5001 \
  get user:1
```

## RPC behaviour

| RPC | Successful missing-key behaviour | Validation failure |
|---|---|---|
| `Put` | Not applicable | `INVALID_ARGUMENT` |
| `Get` | `found=false` | `INVALID_ARGUMENT` |
| `Delete` | `deleted=false` | `INVALID_ARGUMENT` |
| `BatchPut` | Not applicable | `INVALID_ARGUMENT` |
| `Status` | Returns service and storage metrics | Deadline status when expired |

When the bounded worker queue is full, storage RPCs return `RESOURCE_EXHAUSTED`. Requests that
expire before a worker begins execution return `DEADLINE_EXCEEDED`. The transport and service both
enforce configurable message limits.

`BatchPut` validates the entire batch before applying any writes. Once execution begins, entries
are durably written in order; the operation is not a multi-key transaction.

## Network execution path

```text
gRPC request
      |
Transport message-size limit
      |
Application request-size validation
      |
Deadline check
      |
Bounded request queue
      |
Queue full? ---------------- yes ---> RESOURCE_EXHAUSTED
      |
Worker thread
      |
Request validation
      |
WAL append and durability policy
      |
MemTable update
      |
Optional flush and compaction
      |
gRPC response
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

## Status metrics

The `Status` RPC exposes:

- Live key count and latest sequence number
- L0 and L1 SSTable counts
- Cache hits, misses, evictions, and hit ratio
- Completed compactions
- Queued and active requests
- Rejected requests
- Total and failed RPC counts

## Tests

The storage presets run 172 unit and integration tests. Network-enabled presets add five in-process
gRPC integration tests for remote CRUD, batch writes, status, deadlines, concurrent clients, and
graceful durability, for a total of 177 tests.

```bash
ctest --preset debug --output-on-failure
ctest --preset network-release --output-on-failure
```

## Sanitizers

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan --output-on-failure
```

The default sanitizer presets validate the storage engine and dependency-free service runtime.
The network AddressSanitizer preset also runs the five in-process gRPC integration tests:

```bash
cmake --preset network-asan
cmake --build --preset network-asan
ctest --preset network-asan --output-on-failure
```

The network ThreadSanitizer preset builds the gRPC server and client but intentionally runs only the
172 project-owned storage and service-runtime tests:

```bash
cmake --preset network-tsan
cmake --build --preset network-tsan
ctest --preset network-tsan --output-on-failure
```

Ubuntu's prebuilt gRPC and Abseil shared libraries are not compiled with the same ThreadSanitizer
instrumentation as NebulaKV. Running the five in-process RPC tests against those binaries produces
reports inside `libgrpc`, `libgpr`, and `libabsl_graphcycles_internal`, so those external-library
integration tests remain covered by `network-release` and `network-asan` instead. NebulaKV's bounded
executor, request processor, storage concurrency, compaction concurrency, and cache concurrency tests
continue to run under ThreadSanitizer.

## Benchmarks

```bash
cmake --preset benchmark
cmake --build --preset benchmark
./build/benchmark/benchmarks/nebulakv_benchmarks
```

Available benchmark groups include hash-store operations, sorted MemTables, indexed SSTable reads,
cache hits, Bloom-filter negative lookups, and leveled compaction.

## Repository layout

```text
.
├── app/main.cpp
├── client/main.cpp
├── server/main.cpp
├── proto/nebulakv/v1/key_value_service.proto
├── include/nebulakv/
│   ├── network/
│   │   ├── bounded_executor.hpp
│   │   ├── grpc_client.hpp
│   │   ├── grpc_server.hpp
│   │   ├── grpc_service.hpp
│   │   └── request_processor.hpp
│   └── storage engine headers
├── src/network/
├── src/storage engine sources
├── tests/
├── benchmarks/
├── CMakeLists.txt
└── CMakePresets.json
```

## Correctness guarantees

- Acknowledged synchronous writes are recoverable after restart.
- Graceful shutdown flushes accepted writes in every durability mode.
- Incomplete WAL tails and malformed records are handled without process crashes.
- WAL and SSTable checksums detect corruption.
- Sequence numbers and tombstones survive restarts and compaction.
- Bloom filters never classify an inserted key as definitely absent.
- Cached blocks are immutable and safely shared across readers.
- Manifest publication occurs before obsolete SSTables are removed.
- The network queue is bounded, and overload is reported rather than growing memory without limit.
- Invalid requests map to explicit gRPC status codes.
