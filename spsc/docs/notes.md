# SPSC notes (working log)

## Status
- Four try_push/try_pop queues (spin-retry mutex, naive ring, ring, cached ring)
  plus a blocking mutex + condvar queue (push/pop).
- Tests: test_queues.cpp (generic tests shared by the four try_ queues, cached-only
  reload tests, layout static_asserts) and test_mutex_condvar_queue.cpp.
  TSan and ASan builds pass [confirm ASan before keeping this].
- Benchmarks and false-sharing demo done. Whiteboard check done.
- CMake build done: header-only library target, tests, benchmark, ctest,
  SPSC_SANITIZER option.
- Next: DESIGN.md, README, GitHub Actions on x86 Linux.

## Decisions
- Ladder, one change per rung: mutex + condvar baseline -> naive ring (seq_cst,
  no padding) -> ring (acquire/release + padded counters) -> ring with cached
  counters.
- Counters count up forever; slot = counter & (Capacity - 1). All Capacity slots
  usable. Full: write - read == Capacity. Empty: read == write.
- Every queue is bounded to the same capacity, so all behave the same when full.
- The first spin-retry MutexQueue stays in the repo and is covered by
  test_queues.cpp, but is not benchmarked, because it starves under a retry
  loop (see below).
- Cache line size is a compile-time constant (spsc::CACHE_LINE_SIZE in
  constants.hpp): 128 on Apple Silicon, 64 on x86. The padded rings' layout is
  checked by static_asserts in test_queues.cpp (sizeof == 3 * line size).

## Build and test (from spsc/)
    cmake -S . -B build-cmake                    # default build type: Release (-O2 -DNDEBUG)
    cmake --build build-cmake
    ctest --test-dir build-cmake --output-on-failure
    ./build-cmake/bench_spsc --benchmark_out=results/<date>_full.json --benchmark_out_format=json

Sanitizers (separate build folder each; never take benchmark numbers from them):

    cmake -S . -B build-tsan -DSPSC_SANITIZER=thread    # or =address
    cmake --build build-tsan
    ctest --test-dir build-tsan --output-on-failure

- Tests are compiled with -UNDEBUG so assert() stays active in a Release build;
  each test file also has an #error guard if NDEBUG is defined.
- Sanitizer builds shrink the stress size with SPSC_STRESS_N=200000.
- thread and address sanitizers cannot be combined in one build.
- Benchmarks need Release (configure warns otherwise). The CMake Release flags
  match the hand-compiled ones used for all recorded results.

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

Mutex + condvar queue: unit and stress tests (capacity 1, 2, 1024) and a TSan
run are clean. Two compile-level bugs found during review: a missing `()` on
`notify_one` (would have hung) and `size` without `()`.

## Benchmark results
Setup: arm64, macOS 27.0.1, Apple clang version 21.0.0, 8 cores, clang++ -std=c++17 -O2 -DNDEBUG,
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

### Repeat runs
- 2026-10-21 (results/2026-10-21_spin_mutex.json), medians 8 B / 64 B:
  naive 37.0 / 21.5, ring 52.8 / 32.3, cached 79.1 / 94.2.
- 2026-10-22, padded runs of the false-sharing demo: cached 8 B 80.8 and 90.3.
- Most rows reproduce within ~4%. Cached 8 B ranges 79-90 M/s across four runs.
  Day-to-day spread is larger than within-run CV, so quote ratios as approximate.

### First mutex baseline: invalid
The earlier spin-retry mutex queue (try_push/try_pop in a retry loop) gave
21.6 ms median but 2.2 s mean (8 B) and ~27 s per iteration (64 B). Likely lock
starvation: a failed attempt still takes the lock (not verified). Replaced by
the condvar queue above.

### Earlier observation, now weakened
Cached ring faster at 64 B than 8 B was 94 vs 79 M/s on 10-21, but 94 vs 89 on
10-22: within session-to-session variation. Not pursued.

## False-sharing demo (2026-10-22)
Same benchmark, same machine; unpadded build = the headers with `alignas(128)`
removed from spsc_ring.hpp and spsc_ring_cached.hpp (scratch copy, repo
untouched). Order: padded, unpadded, padded. Median M items/s:

| | padded (a / b) | no padding | padded / no padding |
|---|---|---|---|
| Ring 8 B | 52.1 / 50.2 | 46.2 | 1.09x - 1.13x |
| Ring 64 B | 31.0 / 31.5 | 29.3 | 1.06x - 1.07x |
| Cached 8 B | 80.8 / 90.3 | 38.2 | 2.1x - 2.4x |
| Cached 64 B | 93.0 / 93.3 | 47.8 | 1.95x |

Files: results/2026-10-22_padded_a.json, _nopad.json, _padded_b.json.
- Padding helps the plain ring a little (~6-13%, close to the noise for 8 B).
- Without padding, the cached ring loses about half its throughput, and at 8 B
  is no faster than the naive seq_cst ring.
- Explanation (not measured): the plain ring reads the other counter on every
  call, so the line bounces anyway; the cached ring avoids those reads, but
  without padding its own writes invalidate the line the other thread uses.
- Caveats: no-padding layout also puts the counters next to the end of buffer_;
  padded-run drift is 0.2%-3.7% (11.7% for cached 8 B).
- Unexplained: at 8 B, unpadded cached (38.2) is slower than unpadded plain
  ring (46.2); at 64 B it is faster (47.8 vs 29.3).

## Prototype results (before the rewrite)
One-slot-empty scheme, int payload, unbounded mutex queue. Kept for history only;
not comparable in absolute terms to the tables above.
- Mutex 9.6 M items/s, ring 35.9, ring + cached 54.8.
- Instruments, L1D load misses per item: mutex 0.32, ring 3.65, cached 1.77.
  Producer's load misses dropped ~157x with cached counters.

## To do
- docs/DESIGN.md and README.md (build, test, sanitize, benchmark, results table,
  limitations).
- GitHub Actions on x86 Linux (build, ctest, TSan; benchmark only as a sanity check).
- Tag spsc-v1.0 when the docs are done.
- Optional: redo Instruments counters on the final queues (10M items each),
  128 B payload test, move operations on all queues, batching, slot remap,
  latency percentiles.