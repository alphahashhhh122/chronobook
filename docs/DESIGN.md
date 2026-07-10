# ChronoBook v3 Design

## Three Lanes

Hot lane:

```text
FeedMessage -> ReplayEngine -> MatchingEngine -> Fill -> SPSC ring
```

The hot lane owns matching correctness and latency. It uses integer ticks,
intrusive lists, sorted price maps, an id hash, a slab allocator, and an SPSC
queue. It does not perform persistence.

Sharded hot lane:

```text
gateway -> SPSC shard queues -> per-shard worker -> per-symbol MatchingEngine
```

`ShardedEngine` partitions messages by `symbolPacked`. Each shard has its own
worker thread, order pool, and map of per-symbol matching engines. This prevents
cross-symbol matching and keeps mutable order-book state private to one worker.

Persistence lane:

```text
Fill batch -> FillJournal -> TradeStore transaction -> queryable projection
```

The journal is append-only and fixed-record. The store is a queryable projection
that can be rebuilt from the journal. The default store is a portable flat-file
projection; the optional dynamic SQLite path models SQLite ownership
boundaries: connection, statement, statement cache, transaction. Store batches
either commit as a unit or do not appear, but the journal and store are separate
files, so the journal is the source of truth after a projection failure.

Read lane:

```text
reader -> ConnectionPool -> ConnectionLease -> query snapshot
```

The pool is bounded. A lease is move-only and returns its connection exactly once
on destruction. The semaphore has a Linux `futex(2)` implementation and a
condition-variable fallback for non-Linux builds.

## Core Invariants

- The book owns resting orders; the pool owns storage.
- A resting order appears in exactly one price level and one id-map entry.
- `PriceLevel::totalQty` equals the sum of remaining quantity in that level.
- Fills are value records; draining fills bounds memory during replay.
- The SPSC producer writes data before release-storing head; the consumer
  acquire-loads head before reading data.
- A transaction rolls back if commit is not reached.
- A moved-from lease is empty and cannot return a connection twice.
- Binary feed messages are validated before enum casts reach the matching path.
- Pool exhaustion is counted explicitly instead of being hidden inside replay
  results.
- In the sharded path, a symbol is owned by one shard and matched by that
  shard's per-symbol engine; different symbols cannot cross-match.

## Correctness Story

Unit tests check hand-computable scenarios first. Deterministic replay checks
repeatability. `ReferenceMatcher` is a slow vector-based oracle; differential
tests assert the fast engine emits the same fill sequence as the simple matcher,
including randomized feeds with ADD/CANCEL/MODIFY, market orders, and IOC.
Sharded tests check single-symbol equivalence, no cross-symbol matching,
independent fills across symbols, and worker start/stop lifecycle.

## Measurement Story

The benchmarks separate throughput from per-operation latency:

- `replay_demo` measures end-to-end replay throughput.
- `latency_histogram` reports p50/p99/p99.9/max per operation.
- `spsc_vs_mutex` compares the Week-3 mutex baseline with the SPSC handoff.
- `futex_vs_cv` compares semaphore strategies.
- `store_throughput` measures batched persistence throughput.
- `sharded_throughput` compares single-threaded per-symbol matching with
  sharded SPSC-backed worker routing.

Do not quote smoke results as universal latency. Pin the CPU, warm up the run,
record hardware/OS/compiler, and report negative results honestly.
