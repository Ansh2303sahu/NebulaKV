# Benchmark methodology

## Remote workload tool

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

The tool reports operations per second, successful reads and writes, errors, and P50/P95/P99/max
latency. It follows leader redirects through the same client library used by the CLI.

Use `scripts/run-workload.sh` to configure the same workload with environment variables.

## Microbenchmarks

```bash
cmake --preset benchmark
cmake --build --preset benchmark
build/benchmark/benchmarks/nebulakv_benchmarks \
  --benchmark_filter='raft|sstable|compaction' \
  --benchmark_repetitions=5 \
  --benchmark_report_aggregates_only=true
```

The Raft microbenchmarks measure in-memory three-node majority commit and quorum read-barrier cost.
Storage microbenchmarks measure cold SSTable reads, warm block-cache reads, Bloom-filter negatives,
and compaction throughput.

## Reproducibility rules

Record:

- CPU model, memory, operating system, compiler, build type, and filesystem
- durability mode
- node count and placement
- value size, key distribution, client count, and read ratio
- election, heartbeat, RPC, and snapshot settings
- whether data and caches were cold or warm
- repetition count and coefficient of variation

Do not describe a cache microbenchmark speedup as an end-to-end database speedup. Report the exact
paths being compared.

## Distributed experiments

Measure separately:

- steady-state throughput
- P50/P95/P99 latency
- leader-election interruption
- follower catch-up duration
- snapshot installation duration
- throughput during LSM compaction
- behaviour under 1%, 5%, and 10% RPC loss
- recovery after abrupt process termination
