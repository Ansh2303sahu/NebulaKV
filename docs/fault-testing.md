# Fault-testing guide

## Automated cluster smoke test

```bash
scripts/ci-cluster-smoke.sh build/distributed-release
```

The test starts three processes, waits for election, commits and reads data through different
nodes, terminates the leader, waits for a replacement leader, and verifies a post-failover write.

## Leader termination

```bash
scripts/run-three-node.sh
build/distributed-release/nebulakv-cli --host 127.0.0.1 --port 5001 status
kill "$(cat logs/node-1.pid)"   # replace with the current leader PID
```

A replacement leader should appear after an election timeout. A write acknowledged before the
termination must remain readable.

## Quorum loss

Stop two nodes. The remaining node may still report its last role briefly, but writes time out and
quorum-confirmed reads return unavailable. This demonstrates that an isolated member cannot make
progress alone.

## Delayed and dropped Raft RPCs

Start nodes with either option:

```text
--fault-delay-ms 100
--fault-drop-probability 0.10
```

Fault injection is applied only to outgoing internal Raft RPCs. Use deterministic unit tests for
repeatable partitions and message drops; probability-based process testing is intended for stress.

## Follower snapshot catch-up

1. Stop one follower.
2. Commit more entries than `--snapshot-threshold` through the other two nodes.
3. Restart the follower with its original data directory.
4. Verify that it reads the newest value and reports a non-zero snapshot index.

## Corruption checks

The test suite covers checksums and malformed files for WAL, SSTables, manifests, Raft log, and
snapshot data. Corruption is reported as startup failure rather than silently ignored.

## Required invariant checks

Every reliability run should verify:

- no acknowledged write disappears after recovery
- only one leader commits in a term
- no minority partition acknowledges a write
- a restarted node catches up before serving leader reads
- an installed snapshot does not resurrect deleted keys
- conflicting uncommitted suffixes are replaced by the elected leader
