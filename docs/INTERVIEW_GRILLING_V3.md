# ChronoBook v3 Interview Grilling Guide

## Questions You Must Own

Why integer prices?

Matching requires exact ordering and equality. Doubles introduce rounding and
comparison surprises. Prices are ticks.

Why intrusive lists?

FIFO at each price with no node allocation, plus O(1) cancel once the id hash
finds the order pointer.

Why map for prices and unordered_map for ids?

Best bid/ask requires sorted prices; cancel is a point lookup by id.

Explain the slab allocator.

One aligned region, a free list threaded through raw slots, placement new on
allocation, explicit destructor on deallocation, RAII cleanup of the region.

Explain the SPSC memory ordering.

Producer writes the slot, then release-stores head. Consumer acquire-loads head,
then reads the slot, then release-stores tail. No CAS is needed because there is
only one producer and one consumer.

Why not put SQLite in `match()`?

The hot path must not block or syscall. Persistence is a downstream projection
fed by fills after matching.

What does transaction RAII buy you?

If commit is not reached, destruction rolls back the active batch. Exceptions do
not leave a half-applied batch in the query projection.

What is the connection-lease invariant?

Only the live lease owns the checked-out connection. Move transfers ownership.
The destructor returns exactly once; moved-from leases are empty.

How do you know it is correct?

Hand tests, deterministic replay, sanitizer-ready concurrency boundaries, and a
slow reference matcher diffed against the fast engine.

What numbers can you claim?

Only measured numbers with method and machine. Replay throughput is not
per-order latency. Smoke benchmarks are useful for development, not resume
claims unless rerun under controlled conditions.

## Whiteboard Drills

- Draw an order moving from new to resting to partially filled to filled/cancelled.
- Walk a buy limit 100 x 50 against asks at 99, 100, and 101.
- Draw the SPSC ring with head/tail cache-line padding.
- Draw the slab free list before and after three allocations and one free.
- Trace a failed transaction: staged batch, exception, rollback/no visibility.
- Explain why a bigger write pool does not help SQLite writers.
