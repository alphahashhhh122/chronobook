# Resume Claim Status

Use this file as the source of truth before pasting ChronoBook onto a resume.

## Safe Today

- C++17 and CMake.
- Exchange-style matching engine.
- Fixed-point integer prices.
- Price-time priority.
- Custom slab/free-list allocator with RAII ownership.
- Linux code path attempts `mmap(... MAP_HUGETLB ...)` when huge pages are requested.
- Intrusive linked-list price levels.
- Software prefetch in the match loop through `__builtin_prefetch` on GCC/Clang.
- O(1) cancel lookup through `unordered_map`.
- Limit, Market, IOC, partial fills.
- Lock-free SPSC ring buffer.
- Acquire/release atomics.
- Cache-line-padded head/tail indices.
- Sharded multi-symbol matching layer with one worker thread per shard and
  per-symbol `MatchingEngine` instances.
- Tests prove different symbols do not cross-match in the sharded path.
- rdtscp latency benchmark executable.
- Thread-affinity pinning executable path.
- Assert-based correctness suite passes on the current 64-bit Windows build.
- Linux `futex(2)` semaphore implementation exists in source, with Windows
  condition-variable fallback.
- Dynamic SQLite C API path is exposed through `CHRONOBOOK_USE_DYNAMIC_SQLITE`.
- Windows MSVC AddressSanitizer Debug CTest passes in this environment after
  staging the Visual Studio ASAN runtime DLLs.
- Malformed feed messages are rejected before raw enum fields reach the matching
  path.
- Replay reports invalid messages and pool-exhausted dropped ADDs explicitly.
- Fill journals reject invalid existing headers instead of silently
  reinitializing them.
- The trade projection can be rebuilt from the append-only fill journal.
- Randomized reference-matcher differential tests are part of the local suite.
- Sharded throughput benchmark target exists for multi-symbol routing
  experiments.

## Not Safe Yet

- GoogleTest: not used. Tests are assert-based.
- "Clean under ASAN/UBSAN/TSAN": Windows MSVC ASAN passed here, but UBSan and
  TSAN were not run because they require a Linux/Clang/GCC environment.
- "Linux perf stat measured IPC/cache/TLB/branch counters": not run here.
- "Huge pages improved latency": not proven. Current Windows run reports `huge_pages_backed,0`.
- Exact `[[likely]]/`[[unlikely]]` attributes: not used because the project is C++17. The code uses portable branch-prediction macros instead.
- Real Linux `futex(2)` execution: source exists, but it was not run here because
  WSL has no installed Linux distribution.
- Real SQLite execution: the path exists, but the available Windows sqlite DLL is
  not a reliable standalone C API target in this environment.
- "Near-linear sharded scaling": the benchmark harness exists, but scaling
  numbers are machine-dependent and should not be quoted without saved repeated
  runs.

## Resume-Safe Version

```text
ChronoBook - C++17 Low-Latency Matching Engine

* Engineered a C++17 matching engine with price-time priority, fixed-point
  integer prices, intrusive FIFO price levels, RAII slab/free-list allocation,
  sorted bid/ask maps, and average O(1) cancel via an order-id hash index.
* Implemented exchange-style Limit/Market/IOC order flow with partial fills,
  cancel/modify handling, deterministic seeded feed replay, logical-sequence
  fills, malformed-feed validation, and reference-matcher differential tests.
* Added sharded multi-symbol routing with one worker thread per shard,
  SPSC-backed inboxes, per-symbol MatchingEngine instances, and tests covering
  single-symbol equivalence, no cross-symbol matching, independent fills, and
  worker lifecycle.
* Built an off-critical-path durability lane through a lock-free SPSC ring into
  a memory-mapped append-only journal and queryable TradeStore projection; added
  recovery reconciliation, rdtscp latency histograms, and benchmark targets.
```

## Upgrade Needed Before Using The Original Version

Run on Linux or WSL Ubuntu:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCHRONOBOOK_ENABLE_ASAN_UBSAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCHRONOBOOK_ENABLE_TSAN=ON
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure

perf stat -e cycles,instructions,cache-misses,dTLB-load-misses,branch-misses \
  ./build/latency_histogram 200000 10000
```
