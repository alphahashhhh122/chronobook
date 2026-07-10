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

## Resume-Safe Version

```text
C++17, CMake, low-latency data structures, deterministic replay

• Built an exchange-style matching engine with price-time priority, fixed-point
integer prices, a custom slab/free-list allocator with RAII ownership, intrusive
linked-list price levels, software prefetch in the match loop, and O(1) cancel
lookup via unordered_map; supports Limit, Market, and IOC orders with partial
fills.

• Implemented a lock-free SPSC ring buffer using acquire/release atomics and
cache-line-padded head/tail indices; added deterministic binary feed replay,
microstructure analytics, an append-only fill journal, and a reference matcher
for differential correctness testing.

• Added feed validation, explicit replay failure counters, journal/store
recovery checks, randomized reference-matcher differential tests, and
rdtscp-based latency benchmarking with warm-up and thread-affinity pinning;
current 64-bit Windows Release build passes the assert-based correctness suite
and benchmark smoke runs.
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
