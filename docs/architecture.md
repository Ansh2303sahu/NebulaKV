# NebulaKV distributed architecture

NebulaKV is a replicated C++20 key-value database. Each node combines a Raft consensus engine,
a durable LSM-tree state machine, a gRPC API, and an HTTP metrics endpoint.

## Request path

```text
client
  |
  | gRPC Put/Get/Delete/BatchPut/Status
  v
bounded request executor
  |
  +-- follower ------------------------> leader hint / client redirect
  |
  +-- leader write
  |      |
  |      v
  |   persistent Raft log
  |      |
  |      v
  |   AppendEntries to peers
  |      |
  |      v
  |   majority replication
  |      |
  |      v
  |   commit + apply to LSM state machine
  |
  +-- leader read
         |
         v
      current-term quorum confirmation
         |
         v
      MemTables -> Bloom filters -> block cache -> SSTables
```

## Safety invariants

1. A client write is acknowledged only after the entry is replicated to a majority and applied by
   the leader.
2. A committed entry is never replaced by a conflicting entry.
3. A node persists its term and vote before replying to election RPCs.
4. A node restores its state machine from the newest snapshot and the committed suffix of the Raft
   log.
5. A linearizable read is served only by a leader that has confirmed contact with a current-term
   quorum.
6. Snapshot publication and Raft metadata replacement use checksummed, atomic files.
7. An isolated leader cannot commit writes and cannot serve quorum-confirmed reads.
8. The LSM engine never deletes obsolete SSTables before a replacement manifest is durable.

## Raft persistence

Each node stores Raft metadata under `<data-dir>/raft`:

- `hard-state` — current term, voted-for node, commit index, last-applied index
- `log` — checksummed, contiguous entries after the retained snapshot
- `snapshot` — checksummed state-machine image and its last included term/index

The storage implementation writes replacement files, synchronizes them, atomically renames them,
and synchronizes the containing directory.

## State-machine persistence

Committed commands are applied to the existing LSM engine:

- synchronous or batched WAL
- active and immutable sorted MemTables
- indexed and checksummed SSTables
- Bloom filters
- shared byte-bounded LRU block cache
- atomic `CURRENT` and `MANIFEST-*`
- L0-to-L1 compaction

Raft snapshots contain the logical key/value state. Installing a snapshot rebuilds the local LSM
state, after which newer committed log entries are replayed.

## Membership model

Membership is static for this release. Every node is started with the complete list of the other
nodes. Joint-consensus membership changes are deliberately not exposed.

## Consistency model

- Successful writes are linearizable.
- Successful reads are linearizable because the leader performs a quorum barrier first.
- Follower `Status` requests are allowed.
- Client data requests sent to a follower return a leader hint; the bundled client follows it.
- Multi-key `BatchPut` validates the full request before execution but commits entries one at a
  time; it is ordered, not transactional.
