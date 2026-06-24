# Operations guide

## Build

```bash
cmake --preset distributed-release
cmake --build --preset distributed-release
ctest --preset distributed-release --output-on-failure
```

## Start a local three-node cluster

```bash
scripts/run-three-node.sh
```

Nodes listen on ports 5001, 5002, and 5003. Metrics listen on 9101, 9102, and 9103.

```bash
build/distributed-release/nebulakv-cli --host 127.0.0.1 --port 5001 status
build/distributed-release/nebulakv-cli --host 127.0.0.1 --port 5001 put account:1 active
build/distributed-release/nebulakv-cli --host 127.0.0.1 --port 5002 get account:1
```

Stop the cluster with:

```bash
scripts/stop-three-node.sh
```

## Docker Compose

```bash
docker compose build
docker compose up -d
docker compose ps
```

Use a client inside the Compose network so leader addresses remain resolvable:

```bash
docker compose exec node-1 nebulakv-cli --host node-1 --port 5001 status
docker compose exec node-1 nebulakv-cli --host node-2 --port 5001 put docker:key value
docker compose exec node-1 nebulakv-cli --host node-3 --port 5001 get docker:key
```

Metrics are published to the host at:

- node-1: `http://127.0.0.1:9101/metrics`
- node-2: `http://127.0.0.1:9102/metrics`
- node-3: `http://127.0.0.1:9103/metrics`

To remove containers while retaining data:

```bash
docker compose down
```

To remove all cluster volumes:

```bash
docker compose down -v
```

## Graceful shutdown

Send `SIGINT` or `SIGTERM`. The node stops accepting new RPCs, drains accepted work, stops Raft,
and flushes the state machine. Container shutdown allows 15 seconds.

## Restart and recovery

Restart a node with the same node ID, peer list, and data directory. It loads hard state, snapshot,
and retained log before participating in elections. A lagging follower receives AppendEntries or an
InstallSnapshot request from the leader.

Never copy a live node directory into another active node. Every member must have a unique data
directory and node ID.

## Backup

For a consistent offline backup:

1. Stop one follower gracefully.
2. Copy that follower's complete data directory.
3. Restart the follower and wait for it to catch up.

The backup must include both the `raft` directory and the LSM files.

## Metrics and logs

Logs are JSON Lines on standard output. Important fields include event, node ID, leader ID,
address, error, term, and index where applicable.

Prometheus text is available at `/metrics`. Important series include request totals and latency,
Raft role/term/commit index, replication failures, snapshots, queue rejection, cache statistics,
and compaction statistics.
