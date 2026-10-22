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

## Benchmark results
Setup: [chip, macOS, clang version], 8 cores, clang++ -std=c++17 -O2 -DNDEBUG,
capacity 1024, 1M items per iteration, 10 repetitions, real time, threads not
pinned. Primary run: 2026-10-22, load average ~1.8
(results/2026-10-22_full.json). Every run checked item order and count; no
benchmark reported lost, duplicated or reordered items.

Median throughput (million items/s), time CV in brackets:

| Queue | 8 B | 64 B |
|---|---|---|
| Mutex + condvar | 27.6 (81%) | 23.9 (5.1%) |
| Naive ring | 37.7 (2.7%) | 20.6 (3.3%) |
| Ring (acq/rel + padding) | 50.6 (0.2%) | 31.9 (2.4%) |
| Ring + cached counters | 88.6 (1.5%) | 93.7 (0.4%) |

Speedup over mutex + condvar: naive 1.36x / 0.86x, ring 1.83x / 1.33x,
cached 3.21x / 3.92x (8 B / 64 B).
Per rung: naive -> ring 1.34x / 1.54x; ring -> cached 1.75x / 2.94x.

- The naive ring is slower than the condvar queue at 64 B.
- Mutex + condvar at 8 B is bimodal (CV 81%); its CPU time is ~0.8x real time
  (threads sleep), while all rings are 1.0x (they spin).
- Throughput under a saturated stream favours the rings; latency (p50/p99 per
  message) and CPU cost are not measured yet.

### Repeat run (2026-10-21, results/2026-10-21_spin_mutex.json)
Medians (8 B / 64 B): naive 37.0 / 21.5, ring 52.8 / 32.3, cached 79.1 / 94.2.
Most rows reproduce within ~4%; cached 8 B moved 12% (79 vs 89). Day-to-day
spread is larger than within-run CV, so quote ratios as approximate.

### First mutex baseline: invalid
The earlier spin-retry mutex queue (try_push/try_pop in a retry loop) gave
21.6 ms median but 2.2 s mean (8 B) and ~27 s per iteration (64 B). Likely lock
starvation: a failed attempt still takes the lock (not verified). Replaced by
the condvar queue above.

### Earlier observation, now weakened
Cached ring faster at 64 B than 8 B was 94 vs 79 M/s on 10-21, but 94 vs 89 on
10-22: within session-to-session variation. Not pursued.

## Prototype results (before the rewrite)
One-slot-empty scheme, int payload, unbounded mutex queue. Kept for history only;
not comparable in absolute terms to the table above.
- Mutex 9.6 M items/s, ring 35.9, ring + cached 54.8.
- Instruments, L1D load misses per item: mutex 0.32, ring 3.65, cached 1.77.
  Producer's load misses dropped ~157x with cached counters.

## To do
- Padding-off benchmark (false-sharing demo).
- Redo Instruments counters for the final implementations (10M items each).
- 128 B payload test (see observation above).
- Later: move operations on all queues, batching, slot remap, latency percentiles.
- Cleanup: merge the four test files into one templated file, shared cache-line
  constant.