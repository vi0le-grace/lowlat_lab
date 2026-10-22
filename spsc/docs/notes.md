# SPSC notes (working log)

## Decisions
- Four rungs, one change each: mutex queue -> naive ring (seq_cst, no padding)
  -> ring (acquire/release + padded counters) -> ring with cached counters.
- Counters count up forever; slot = counter & (Capacity - 1). All Capacity slots
  usable. Full: write - read == Capacity. Empty: read == write.
- Every queue is bounded to the same capacity, so all behave the same when full.
- Cache line size hardcoded to 128 (Apple Silicon); to be moved into one constant.

## Correctness experiments (break it on purpose)
Each change was made temporarily, then reverted.

| Queue | Change | Caught by |
|---|---|---|
| Naive ring | write_counter_.store moved above the buffer write | TSan: data race on buffer_ (normal tests still passed) |
| Ring | each of the 3 release/acquire ops weakened to relaxed | TSan, all three |
| Cached ring | reload acquire weakened to relaxed | TSan (3 warnings); unit tests did not notice |
| Cached ring | inner re-check deleted in try_push | test_fill_and_full (push on a full queue succeeded) |
| Cached ring | cached copy never updated | test_wraparound (push refused on a non-full queue) |

Lesson: logic bugs are caught by deterministic tests; ordering bugs only by TSan.

## Benchmark results (2026-10-21)
Setup: [chip, macOS, clang version], 8 cores, clang++ -std=c++17 -O2 -DNDEBUG,
capacity 1024, 1M items per iteration, 10 repetitions, real time, threads not
pinned, load average ~2.4. Raw data: results/results.json.
Every run checked item order and count; no benchmark reported lost, duplicated
or reordered items.

Median throughput (million items/s) and spread (CV of time):

| Queue | 8 B payload | 64 B payload |
|---|---|---|
| Naive ring | 37.0 (10.4%) | 21.5 (3.3%) |
| Ring (acq/rel + padding) | 52.8 (6.5%) | 32.3 (4.9%) |
| Ring + cached counters | 79.1 (2.5%) | 94.2 (1.0%) |

Speedup per rung:

| Step | 8 B | 64 B |
|---|---|---|
| Naive -> ring | 1.43x | 1.50x |
| Ring -> cached | 1.50x | 2.92x |
| Naive -> cached | 2.14x | 4.38x |

- The cached ring is also the steadiest (lowest CV) in both sizes.
- The ring -> cached ratio at 8 B (1.50x) matches the prototype (1.53x).

### Mutex queue: results invalid
- 8 B: median 21.6 ms per 1M items, but mean 2.2 s and CV 267% (about one of ten
  repetitions took ~22 s).
- 64 B: ~27 s per 1M items in every repetition.
- Likely cause (not verified): both threads spin-retry on try_push/try_pop and a
  failed attempt still takes the lock, so one thread can starve the other. This
  is a property of the benchmark loop, not the queue's design. Do not quote these
  numbers; replace with a condition-variable baseline.

### Unexplained observation
The cached ring is faster with 64 B items than 8 B (94 vs 79 M/s), while the
other two rings slow down. Hypothesis (unverified): with 8 B items 16 slots share
one 128-byte line, so a nearly empty queue makes producer and consumer fight over
the same data line; 64 B items put only 2 per line. Test: add a 128 B payload and
compare load misses per item in Instruments.

## Prototype results (before the rewrite)
One-slot-empty scheme, int payload, unbounded mutex queue. Kept for history only;
not comparable in absolute terms to the table above.
- Mutex 9.6 M items/s, ring 35.9, ring + cached 54.8.
- Instruments, L1D load misses per item: mutex 0.32, ring 3.65, cached 1.77.
  Producer's load misses dropped ~157x with cached counters.

## To do
- Mutex + condition variable baseline (blocking push/pop), rerun mutex rows.
- Padding-off benchmark (false-sharing demo).
- Redo Instruments counters for the final implementations (10M items each).
- 128 B payload test (see observation above).
- Later: move operations on all queues, batching, slot remap, latency percentiles.
- Cleanup: merge the four test files into one templated file, shared cache-line
  constant.