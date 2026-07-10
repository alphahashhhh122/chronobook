# ChronoBook — Weeks 3 & 4 Design Notes

> Legacy note: this file documents the Week 3/4 design narrative from the
> earlier project stage. Treat any sanitizer/perf claims here as study notes
> unless they also appear in `docs/RESUME_CLAIMS.md` under "Safe Today".
> The current verified source of truth is `docs/RESUME_CLAIMS.md`.

## Week 3 — Feed, Parser, Generator, Concurrency

### BinaryProtocol (`FeedMessage`, 40 bytes)
- One fixed-width message type carries ADD / CANCEL / MODIFY (a `msgType` tag).
  Fixed width makes framing trivial: message N is at byte `N * 40`. No length
  prefixes, no escaping.
- **Serialization is whole-struct `memcpy`** to/from a `std::byte` buffer. memcpy
  is the only strict-aliasing-safe way to turn raw bytes into a
  trivially-copyable struct. We never `reinterpret_cast<FeedMessage*>(bytes)`.
- `static_assert(sizeof == 40)` + `static_assert(is_trivially_copyable)` make the
  contract compiler-checked. Explicit `_pad[5]` removes any implementation-defined
  padding, so the layout is identical across compilers.
- **Endianness:** host-endian, because this is a same-machine replay format. A
  cross-host feed would byte-swap each field to a fixed wire endianness.

### FeedParser
- `parseBuffer` returns one message per 40 bytes and reports `bytesConsumed`,
  leaving any trailing **partial frame** unconsumed — the behavior you need when
  bytes arrive in arbitrary chunks (socket/file reads can split a message).
- File round-trip (`writeFile`/`readFile`) is tested byte-exact.

### FeedGenerator (deterministic)
- Seeded `std::mt19937_64`. Same seed ⇒ byte-identical stream. We deliberately
  never touch `std::random_device` or wall-clock — both would break determinism,
  and determinism is what lets you diff two runs (and Week 6's reference matcher).
- Models realistic flow: mostly resting limits, some marketable crosses,
  occasional market/IOC, and cancels of previously-added ids. A cancel may target
  an already-filled id — a real race; the engine returns `false` and the replay
  counts it as a `cancelMiss`.

### ThreadSafeQueue (mutex + condition_variable)
- "Correct first" baseline; Week 5 swaps in a lock-free SPSC ring buffer and
  benchmarks the two.
- `wait(lk, predicate)` — predicate loop, **not** a bare `if`, to absorb spurious
  wakeups.
- `notify_*` is called **outside** the lock so the woken thread doesn't instantly
  re-block on a mutex we still hold.
- Shutdown via `close()`: `pop()` returns `nullopt` only when the queue is **both
  closed and empty**, so the consumer drains every queued item before exiting —
  no lost messages, no deadlock.
- Producer→consumer feeding the engine produces **the exact same book + fills** as
  the single-threaded replay (FIFO queue + one consumer preserves order ⇒
  determinism). This is asserted in the test suite. Run the sanitizer CMake
  targets in `docs/RESUME_CLAIMS.md` before claiming TSAN-clean.

## Week 4 — Replay & Analytics

### ReplayEngine (two modes)
- **NORMAL**: apply each message, drain fills, sample all four analytics off the
  (const) book. For correctness + microstructure inspection.
- **MAX**: apply as fast as possible with a warm-up discard; measure end-to-end
  throughput (msgs/sec). Warm-up matters because the first thousands of ops run
  on cold caches and an untrained branch predictor.
- Determinism: messages applied strictly in feed order by one thread ⇒ the book
  and fills are a pure function of the input.
- Lifetime: ADD allocates from the pool and hands the Order to the engine, which
  then owns it (rests or frees). ReplayEngine never double-frees. MODIFY = cancel
  + re-add (a real price/qty change loses time priority — standard exchange
  semantics).

### RunningStats (Welford)
- Single-pass mean/variance; O(1) memory over millions of samples; numerically
  stable (the naive Σx² − (Σx)²/n formula loses precision and can go negative).
- Reused by every analytics tracker.

### Trackers
- **Spread** = ask − bid (skip when a side is empty).
- **QueueDepth** = bidQty + askQty at top of book (thin queue ⇒ price moves easily).
- **FillRate** = windowed fraction of incoming orders that traded (ring buffer of
  recent outcomes) + cumulative.
- **Imbalance** = (bidQty − askQty)/(bidQty + askQty) ∈ [−1,1]; > 0 ⇒ bid-heavy ⇒
  short-term up-pressure. Simplest microstructure alpha signal.
