# ChronoBook — Interview Grilling Guide (Weeks 3 & 4)

> Legacy note: this file predates the v3 audit. It is useful for interview
> practice, but do not treat its sanitizer/perf wording as resume-safe unless
> the same claim is listed as safe in `docs/RESUME_CLAIMS.md`.

How to use this: read the question, answer OUT LOUD before reading the answer,
then check. The "Follow-up" lines are what a Graviton / IMC / DE Shaw interviewer
actually asks after your first answer. If you can't survive the follow-up, you
don't own the topic yet.

──────────────────────────────────────────────────────────────────────────────
## A. BINARY PROTOCOL & SERIALIZATION

**Q1. Why a custom binary protocol instead of ITCH/PITCH or JSON/Protobuf?**
ITCH/PITCH drags in network-format and exchange-spec knowledge that isn't the
point of this project; JSON/Protobuf add parsing cost and variable-width framing.
A fixed-width custom format keeps the focus on layout and zero-overhead
serialization. Every message is 40 bytes, so message N is at offset N×40 —
framing is arithmetic, not parsing.

**Q2. How do you serialize, and why is it safe?**
Whole-struct `memcpy` into a `std::byte` buffer, and `memcpy` back out. The struct
is `static_assert`-ed trivially copyable, so its object representation is just its
bytes. memcpy is the *only* strict-aliasing-safe way to reinterpret raw bytes as a
struct — casting a `char*` to `FeedMessage*` and dereferencing is undefined
behavior (aliasing + alignment).
- **Follow-up: "Isn't a cast faster than memcpy?"** No — the compiler lowers a
  fixed-size memcpy of a trivially-copyable type to the same loads/stores as a
  cast, but without the UB. Zero runtime cost, full correctness.

**Q3. What's the `_pad[5]` for? What breaks without it?**
Without explicit padding the compiler inserts its own to satisfy alignment, and
the amount is implementation-defined — two compilers could disagree on layout,
breaking the wire format and the `sizeof==40` contract. Explicit pad makes the
layout deterministic and self-documenting.

**Q4. Your format is host-endian. When is that a bug?**
Only across machines of different endianness. This is a same-host replay protocol,
so it's fine. For a real cross-host feed I'd byte-swap each integer field to a
fixed endianness (e.g. little-endian) on encode and back on decode.

──────────────────────────────────────────────────────────────────────────────
## B. PARSER & FRAMING

**Q5. A read gives you 95 bytes (two 40-byte messages + 15 leftover). What happens?**
`parseBuffer` returns 2 messages and sets `bytesConsumed = 80`. The 15-byte
partial frame is left for the caller to prepend to the next read. Fixed width
makes this trivial: `count = len / 40`, remainder is the partial.
- **Follow-up: "Why not just assume whole messages?"** Because byte streams
  (sockets, large files read in chunks) split messages arbitrarily. Assuming whole
  frames silently corrupts everything after the first split.

──────────────────────────────────────────────────────────────────────────────
## C. DETERMINISM & THE GENERATOR

**Q6. Why does the feed generator have to be deterministic?**
Reproducibility. Same seed ⇒ identical stream ⇒ identical book and fills every
run. That's what lets me (a) diff two engine versions to catch regressions and (b)
diff the fast engine against the slow reference matcher (Week 6) — the strongest
correctness argument. A non-deterministic feed makes "is it still correct?"
unanswerable.

**Q7. What would silently destroy determinism?**
`std::random_device`, seeding from wall-clock, any wall-clock timestamp written
into the data, unordered iteration that leaks into output, or multi-threaded
application of orders without a total order. That last one is why the matching
path is single-threaded.

**Q8. Your generator emits cancels for orders that may already be filled. Bug?**
No — it's realistic. Cancels race fills in real markets. The engine looks the id
up, doesn't find it, returns `false`, and the replay counts a `cancelMiss`. Tests
assert that path is hit and handled gracefully.

──────────────────────────────────────────────────────────────────────────────
## D. CONCURRENCY — ThreadSafeQueue

**Q9. Walk me through your queue. Why mutex+condvar and not lock-free yet?**
"Correct first, fast later." Mutex + `condition_variable` is obviously correct and
easy to reason about; it's the baseline I benchmark the Week-5 lock-free SPSC ring
buffer *against*. Producer pushes under the lock and notifies; consumer waits on a
predicate and pops.

**Q10. Why `wait(lk, predicate)` and not `if (empty) wait()`?**
Spurious wakeups: a condvar can return from `wait` without a `notify`. With a bare
`if`, the thread proceeds and pops an empty queue. The predicate form re-checks the
condition in a loop, so a spurious wakeup just goes back to sleep.

**Q11. Why notify outside the lock?**
If I notify while holding the mutex, the woken consumer wakes up and immediately
blocks trying to acquire the mutex I still hold — a wasted wake and context switch
("hurry up and wait"). Releasing first lets it actually run.

**Q12. How does shutdown work without losing messages or deadlocking?**
`close()` sets a flag and `notify_all()`. `pop()`'s predicate is
`!empty || closed`, but it only returns `nullopt` when the queue is **closed AND
empty**. So after close, consumers keep draining real items and only see the
sentinel once everything is processed. No lost messages; no consumer parked
forever.

**Q13. The engine is single-threaded but you have a producer/consumer. Why? Is it deterministic?**
The queue decouples *feed I/O / parsing* (producer) from *matching* (single
consumer). Matching stays single-threaded for determinism; the FIFO queue plus
exactly one consumer preserves message order, so the result is byte-identical to
the single-threaded replay. I assert that equivalence in the test suite, and it
is designed to be checked under TSAN.
- **Follow-up: "So the concurrency buys you nothing here?"** In this harness it's
  about clean separation and being sanitizer-ready; the real latency win is Week 5's
  lock-free SPSC, where the point is removing the mutex from the hot handoff.

**Q14. How do you know it's actually thread-safe — not just 'passed once'?**
ThreadSanitizer instruments every memory access and synchronization edge and flags
data races even if they didn't manifest in that run. The concurrent test (two
threads, 10k items, checksum) and the producer/consumer equivalence test are the
tests to run under TSAN before making a sanitizer-clean claim.

──────────────────────────────────────────────────────────────────────────────
## E. REPLAY ENGINE

**Q15. Difference between NORMAL and MAX mode?**
NORMAL applies each message and samples analytics off the book — for correctness
and microstructure inspection. MAX skips analytics, warms up, and times the rest
to report throughput. You separate them so analytics overhead doesn't pollute the
throughput number.

**Q16. Why warm-up before timing?**
The first N operations run on cold instruction/data caches and an untrained branch
predictor, so they're slower and not representative of steady state. Discarding
them measures steady-state throughput.

**Q17. Who owns an Order's memory across a replay? Where could you double-free?**
Pool → ReplayEngine (briefly, for ADD) → engine → book (if it rests) → back to
pool. The double-free trap: if ReplayEngine freed the Order after
`processOrder`, it would free an Order the book is still holding. So ReplayEngine
hands ownership to the engine and never frees — the engine frees on full
fill / market / IOC residual, or the book holds it until cancel.

**Q18. How do you model a MODIFY?**
Cancel + re-add. A genuine price or quantity change loses time priority — the order
goes to the back of the (possibly new) price level's FIFO queue. That's standard
exchange semantics; pretending a modify keeps its place would be wrong.

──────────────────────────────────────────────────────────────────────────────
## F. ANALYTICS & MARKET MICROSTRUCTURE

**Q19. Why Welford's algorithm for the running stats?**
Single pass, O(1) memory over millions of samples, and numerically stable. The
textbook variance = (Σx² − (Σx)²/n)/(n−1) subtracts two large nearly-equal numbers
→ catastrophic cancellation, and can even produce a negative "variance." Welford
updates the mean and the sum-of-squared-deltas incrementally and avoids that.

**Q20. What is order book imbalance and why track it?**
`(bidQty − askQty)/(bidQty + askQty)`, in [−1, 1]. Positive means the bid side is
heavier — more buyers resting than sellers — which tends to precede a short upward
tick. It's one of the simplest alpha signals in market microstructure, and it
shows I think of the book as *information*, not just a matching structure. Five
lines of code, big signal in an interview.
- **Follow-up: "Why top-of-book only, not full depth?"** Top of book is where most
  of the predictive signal lives and it's O(1). A depth-weighted imbalance over k
  levels is a natural extension; I kept it to L1 for clarity.

**Q21. What does fill rate tell you and why windowed?**
Fraction of incoming orders that trade. High ⇒ aggressive, marketable flow
crossing the spread; low ⇒ mostly passive resting. Windowed (ring of recent
outcomes) so it reflects *current* regime, not a whole-run average that smears
bursts.

**Q22. Why is queue depth interesting separately from spread?**
Spread is the *price* gap; depth is the *size* resting at the top. A tight spread
with a thin queue is fragile — a single order can clear the level and move the
price. Depth is the liquidity dimension spread alone misses.

──────────────────────────────────────────────────────────────────────────────
## G. CORRECTNESS STORY (ties Weeks 3–4 together)

**Q23. How do you know the whole replay pipeline is correct?**
Layers: (1) assert-based unit tests across all components; (2) protocol
round-trip and partial-frame tests; (3) sanitizer CMake targets to run before
claiming ASAN/UBSAN/TSAN clean; (4) a determinism test — same feed twice gives
identical book + fills; (5) a producer/consumer-equals-single-threaded test; and
(6) a slow reference matcher diffed against the fast engine.

──────────────────────────────────────────────────────────────────────────────
## H. HONESTY GUARDRAILS (do NOT bluff these)
- Throughput is *replay throughput on my machine*, not sub-µs HFT match latency.
  Per-op p50/p99/p99.9 is Week 6 and not yet measured — say so.
- Huge-page / prefetch / branch-hint before-after numbers are Week 6. If asked
  "by how much did huge pages help?", the honest answer until measured is "I
  haven't measured it yet; here's the metric I'd expect to move (dTLB-load-misses)
  and how I'd verify it with perf stat." Never invent a percentage.
- The concurrency here is a clean decoupling and has sanitizer targets, not yet the
  lock-free win — that's Week 5.
