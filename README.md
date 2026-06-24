# NebulaKV

NebulaKV is a C++20 distributed key-value database built around a Raft-replicated LSM-tree storage
engine. It combines persistent consensus, linearizable client operations, crash recovery, snapshots,
checksummed storage formats, compaction, observability, fault injection, and reproducible testing in
one repository.

## Highlights

- Three-node Raft consensus with follower, candidate, and leader states
- Randomized elections, heartbeats, majority commit, and conflicting-log repair
- Persistent term, vote, commit index, replicated log, and state-machine snapshots
- Snapshot installation for lagging followers and retained-log truncation
- Linearizable writes and quorum-confirmed leader reads
- gRPC `Put`, `Get`, `Delete`, `BatchPut`, `Status`, and internal Raft RPCs
- Automatic client leader redirects
- Fixed worker pool, bounded request queue, deadlines, and backpressure
- WAL, sorted MemTables, indexed SSTables, Bloom filters, LRU block cache, and compaction
- Atomic `CURRENT`, manifest, Raft metadata, and snapshot publication
- JSON Lines logs and Prometheus-compatible metrics
- Deterministic in-memory partitions, drops, and latency injection
- Remote workload generator and Google Benchmark microbenchmarks
- Local-process and Docker Compose three-node deployments
- GCC, Clang, GoogleTest, sanitizers, formatting, and GitHub Actions

## Architecture

```text
Remote client
     |
     v
leader-aware gRPC client
     |
     v
bounded service executor
     |
     +---- follower --------------------> leader hint
     |
     +---- leader write
     |        |
     |        v
     |   persistent Raft log
     |        |
     |        v
     |   majority replication
     |        |
     |        v
     |   commit and apply
     |
     +---- leader read
              |
              v
        current-term quorum barrier
              |
              v
     WAL / MemTables / SSTables / cache
```

See [architecture](docs/architecture.md), [operations](docs/operations.md),
[fault testing](docs/fault-testing.md), and [benchmarking](docs/benchmarking.md).

## Prerequisites

```bash
sudo apt update
sudo apt install -y \
  build-essential \
  clang \
  clang-format \
  clang-tidy \
  cmake \
  git \
  ninja-build \
  protobuf-compiler \
  protobuf-compiler-grpc \
  libprotobuf-dev \
  libgrpc++-dev
```

CMake 3.22 or newer and a C++20 compiler are required.

## Build and test the distributed system

```bash
cmake --preset distributed-release
cmake --build --preset distributed-release
ctest --preset distributed-release --output-on-failure
```

The complete test corpus contains 190 unit and integration tests. Network-enabled builds generate
Protocol Buffer and gRPC sources from `proto/nebulakv/v1/key_value_service.proto`.

## Start three local nodes

```bash
scripts/run-three-node.sh
```

The nodes use:

| Node | Client/Raft endpoint | Metrics |
|---|---:|---:|
| node-1 | 127.0.0.1:5001 | 127.0.0.1:9101 |
| node-2 | 127.0.0.1:5002 | 127.0.0.1:9102 |
| node-3 | 127.0.0.1:5003 | 127.0.0.1:9103 |

Check status and use the database:

```bash
build/distributed-release/nebulakv-cli \
  --host 127.0.0.1 --port 5001 status

build/distributed-release/nebulakv-cli \
  --host 127.0.0.1 --port 5002 put user:1 Ansh

build/distributed-release/nebulakv-cli \
  --host 127.0.0.1 --port 5003 get user:1

build/distributed-release/nebulakv-cli \
  --host 127.0.0.1 --port 5001 delete user:1
```

The client follows leader metadata when the first node contacted is a follower.

Stop all local nodes:

```bash
scripts/stop-three-node.sh
```

## Start a node manually

```bash
build/distributed-release/nebulakv-node \
  --node-id node-1 \
  --listen 0.0.0.0:5001 \
  --advertise 127.0.0.1:5001 \
  --peer node-2=127.0.0.1:5002 \
  --peer node-3=127.0.0.1:5003 \
  --data-dir data/cluster/node-1 \
  --metrics-port 9101 \
  --durability sync
```

Important options:

```text
--workers <count>
--queue-capacity <count>
--max-message-bytes <bytes>
--election-min-ms <milliseconds>
--election-max-ms <milliseconds>
--heartbeat-ms <milliseconds>
--rpc-timeout-ms <milliseconds>
--snapshot-threshold <committed entries>
--fault-drop-probability <0..1>
--fault-delay-ms <milliseconds>
```

## Consistency and failure behaviour

A write is successful only after a majority has stored the entry and the leader has applied it.
An isolated leader cannot acknowledge writes. `Get` and the existence check used by `Delete` require
a fresh current-term quorum confirmation, so an isolated former leader cannot serve stale successful
reads.

The bundled cluster smoke test verifies election, replication, cross-node reads, leader termination,
replacement election, and post-failover writes:

```bash
scripts/ci-cluster-smoke.sh build/distributed-release
```

## Snapshots

Every node creates a logical state-machine snapshot after the configured number of applied entries.
The snapshot records its last included Raft term and index. Older log entries are removed after the
snapshot is durable. A follower whose next required entry is older than the retained log receives an
`InstallSnapshot` RPC and then resumes normal replication.

Node data is organized as:

```text
data/cluster/node-1/
├── raft/
│   ├── hard-state
│   ├── log
│   └── snapshot
├── state.wal
└── sstables/
    ├── CURRENT
    ├── MANIFEST-...
    └── *.sst
```

## Metrics and logs

Each node exposes Prometheus text at `/metrics`:

```bash
curl http://127.0.0.1:9101/metrics
```

Metrics include:

- request totals, failures, redirects, and latency percentiles
- queue depth, active work, and rejected requests
- Raft role, term, commit index, last-applied index, and replication lag
- election, replication-failure, snapshot-created, and snapshot-installed counts
- cache, Bloom-filter, compaction, and storage statistics

Node logs are JSON Lines on standard output.

## Workload benchmark

```bash
build/distributed-release/nebulakv-benchmark \
  --host 127.0.0.1 \
  --port 5001 \
  --duration-seconds 30 \
  --clients 16 \
  --keyspace 100000 \
  --value-bytes 256 \
  --read-ratio 0.80
```

Or:

```bash
CLIENTS=32 DURATION_SECONDS=60 READ_RATIO=0.95 scripts/run-workload.sh
```

The report includes operations per second, errors, successful reads and writes, and
P50/P95/P99/maximum latency.

## Microbenchmarks

```bash
cmake --preset benchmark
cmake --build --preset benchmark

build/benchmark/benchmarks/nebulakv_benchmarks \
  --benchmark_filter='raft|sstable|compaction' \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true
```

The benchmark target covers majority commit, quorum read barriers, cold and cached SSTable reads,
Bloom-filter negatives, and compaction throughput.

## Docker Compose

```bash
docker compose build
docker compose up -d
docker compose ps
```

Use the CLI inside the Compose network:

```bash
docker compose exec node-1 \
  nebulakv-cli --host node-2 --port 5001 put docker:key value

docker compose exec node-1 \
  nebulakv-cli --host node-3 --port 5001 get docker:key
```

Host metrics are available on ports 9101, 9102, and 9103. Persistent named volumes keep node data
across `docker compose down`; use `docker compose down -v` to delete it.

## Sanitizers and formatting

```bash
cmake --preset distributed-asan
cmake --build --preset distributed-asan
ctest --preset distributed-asan --output-on-failure

cmake --preset distributed-tsan
cmake --build --preset distributed-tsan
ctest --preset distributed-tsan --output-on-failure

cmake --preset debug
cmake --build --preset debug --target format
cmake --build --preset debug --target format-check
```

The ThreadSanitizer preset keeps the real server and client targets enabled while excluding the
in-process gRPC test target when the system gRPC runtime is not sanitizer-compatible.

## Storage-only build

The LSM engine and Raft core do not require gRPC:

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release --output-on-failure
```

The local storage administration executable is `build/release/nebulakv-storage`.

## Current scope

- Static cluster membership
- Unary gRPC RPCs
- Leader-based linearizable reads and writes
- One logical state machine per node
- L0/L1 storage compaction

Dynamic Raft membership, TLS identity management, multi-key transactions, and geographic
multi-region tuning are intentionally outside this release.

## License

See [LICENSE](LICENSE).
