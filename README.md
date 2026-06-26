# ChronoBook

ChronoBook is a C++17 low-latency limit order book and matching engine. It
keeps the matching path in memory, exposes fills as drained batches, and
persists/query-projects fills outside the critical path.

The main design constraint is separation: order matching should not block on
I/O, database work, or downstream analytics.

## Features

- Price-time-priority matching for limit, market, and IOC orders.
- Partial fills, maker-price execution, and FIFO queues at each price level.
- Fixed-point integer prices and compact 48-byte `Order` records.
- Intrusive price levels for O(1) insertion/removal within a level.
- O(1) cancel lookup through an order-id hash table.
- Reusable slab/free-list order allocator with RAII cleanup.
- Lock-free SPSC ring buffer benchmark using acquire/release atomics and
  cache-line padded indices.
- Deterministic binary feed generation, parsing, and replay.
- Memory-mapped append-only fill journal and queryable trade projection.
- Optional SPSC durability pipeline from drained fills into journal + store.
- Event-sourced recovery reconciler for feed replay, journal, and store output.
- Reference matcher used as a slower correctness oracle.
- CMake targets for tests, replay tools, and benchmark executables.

## Architecture

The hot path is intentionally small:

```text
FeedMessage -> ReplayEngine -> MatchingEngine -> OrderBook
                                      |
                                      v
                                   Fill batch
                                      |
                                      v
                    SPSC durability pipeline
                                      |
                                      v
                     mmap journal / projection / analytics
```

Core components:

- `OrderBook` stores bid levels in descending price order and ask levels in
  ascending price order. The best bid/ask is always the first map entry.
- `PriceLevel` owns the FIFO queue for one price using pointers embedded inside
  `Order`, so no extra list node allocation is needed.
- `MatchingEngine` owns matching semantics and emits value-type `Fill` records.
- `SPSCRingBuffer` is a lock-free one-producer/one-consumer handoff primitive
  with a mutex queue comparison benchmark.
- `FillJournal` writes fixed-size binary fill records through a memory-mapped
  append region for deterministic replay.
- `DurabilityPipeline` consumes fill batches through an SPSC ring and persists
  them to the journal and trade projection off the matching path.
- `RecoveryReconciler` replays the deterministic feed and checks that recovered
  fills match the journal and trade-store projection.
- `TradeStore` batches fills into a queryable projection and exposes VWAP and
  time-range query helpers.
- `ConnectionPool` uses move-only leases so read-side handles return safely to
  the pool.

## Trade Store Backends

`TradeStore` has two backends behind a compile flag:

- Default: dependency-free flat-file projection. `insertBatch` stages the
  updated vector and rewrites the file on commit; `vwap` scans the stored fills;
  `explainPlan` returns a portable index-plan string used by the tests.
- `CHRONOBOOK_USE_DYNAMIC_SQLITE=ON`: dynamically loads `sqlite3` at runtime,
  enables WAL mode, wraps batches in `BEGIN IMMEDIATE` / `COMMIT` /
  `ROLLBACK`, uses prepared statements through an LRU cache, and exposes the
  real SQLite `EXPLAIN QUERY PLAN` output.

The dynamic path is loaded at runtime so the default build has no link-time
SQLite dependency.

## Build And Test

ChronoBook requires a 64-bit C++17 compiler and CMake.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The current Windows Release build passes the assert-based test suite:

```text
100% tests passed, 0 tests failed out of 1
```

The tests cover order layout, allocator reuse, price-level FIFO behavior,
matching semantics, parser framing, deterministic replay, queue handoff,
analytics, latency histograms, mmap journal replay, SPSC durability pipeline,
feed/journal/store recovery reconciliation, trade-store rollback, connection
lease behavior, and reference-matcher differential output.

## Benchmark Targets

```bash
./build/Release/replay_demo.exe 1000000
./build/Release/latency_histogram.exe 20000 2000
./build/Release/spsc_vs_mutex.exe
./build/Release/futex_vs_cv.exe
./build/Release/store_throughput.exe
./build/Release/pagefaults.exe
```

The benchmarks cover SPSC-vs-mutex handoff throughput, deterministic replay
throughput, per-operation `rdtscp` latency, semaphore behavior, page-fault
effects, and trade-store insert/query throughput. Results are machine-dependent;
the programs print the command-line workload and measured rates so runs can be
recorded with the environment that produced them.

## Repository Layout

```text
include/core/       order records, slab pool, price levels, order book
include/matching/   matching engine and reference matcher
include/feed/       binary feed protocol, parser, deterministic generator
include/infra/      queues, SPSC ring, semaphore primitives
include/journal/    mmap fill journal, durability pipeline, replay helpers
include/replay/     deterministic replay, recovery checks, latency utilities
include/store/      trade projection and connection pool
bench/              benchmark and smoke-test executables
tools/              feed generator and trade query CLIs
tests/              assert-based correctness suite
```
