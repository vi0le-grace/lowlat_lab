# SPSC notes (working log)

## Status
- Four try_push/try_pop queues (spin-retry mutex, naive ring, ring, cached ring)
  plus a blocking mutex + condvar queue (push/pop), all with move overloads.
- Tests: test_queues.cpp (generic tests shared by the four try_ queues, cached-only
  reload tests, move tests, layout static_asserts) and
  test_mutex_condvar_queue.cpp. TSan and ASan builds pass.
- Benchmarks done: throughput (bench_spsc), false-sharing demo, latency
  (bench_latency, ping-pong round trip), Instruments counters (bench_trace,
  ping-pong and stream). Whiteboard check done.
- CMake build done: header-only library target, tests, bench_spsc, bench_latency,
  bench_trace, ctest, SPSC_STRESS_N, SPSC_SANITIZER option.
- docs/DESIGN.md written (sections 11 and 12 cover latency and counters).
- Next: README, GitHub Actions on x86 Linux, tag spsc-v1.0.

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
- Move overloads on all queues: a failed try_push leaves its argument intact.
  Slots are an array of live objects, so T must be default-constructible and
  assignable; no emplace (would need raw storage, future work).
- Known gap: the benchmark and stress tests check only the sequence number /
  ints, so a torn copy of a larger item would not be detected.
- bench_trace exists only for profiling: main joins two worker threads, so each
  thread's counters contain only queue operations.

## Build and test (from spsc/)
    cmake -S . -B build-cmake                    # default build type: Release (-O2 -DNDEBUG)
    cmake --build build-cmake
    ctest --test-dir build-cmake --output-on-failure
    ./build-cmake/bench_spsc --benchmark_out=results/<date>_full.json --benchmark_out_format=json
    ./build-cmake/bench_latency [all|ring|naive|cached|condvar]
    ./build-cmake/bench_trace <pingpong|stream> <ring|naive|cached> [n]

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

Instruments (CPU Counters, manual events):
- Sign the binary after every rebuild:
  `codesign -s - -v -f --entitlements ~/debug.entitlements build-cmake/bench_trace`
  (entitlement: com.apple.security.get-task-allow). Check with
  `codesign -d --entitlements - <binary>`.
- One run per (workload, queue), arguments e.g. `pingpong cached` or `stream ring`;
  never `all` (it would mix queues). Do not select a time range. Expand the
  process row to get one row per thread, named by function.
- Events used: cycles, instructions, L1D_CACHE_MISS_LD_NONSPEC (retired loads),
  L1D_CACHE_MISS_LD (all loads), L1D_CACHE_MISS_ST, BRANCH_MISPRED_NONSPEC.
  Using the _NONSPEC variant for both load and store misses plus
  BRANCH_MISPRED_NONSPEC gave "conflicting events" and the run did not start;
  the plain L1D_CACHE_MISS_ST works.
- Instruments prints numbers with Indian digit grouping (lakh/crore, from the
  system region setting): 1,27,66,08,436 = 1,276,608,436.

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
- Throughput under a saturated stream favours the rings. Latency is in the next
  section; CPU cost is not measured.

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

## Latency, ping-pong round trip (bench_latency)
Thread A pushes into `ping`, thread B echoes through `pong`, A times the round
trip; one message in flight. 1M round trips after 10k warm-up, capacity 1024,
uint64_t payload, three runs (results/latency_run1.txt .. latency_run3.txt), ns:

| | p50 | p90 | p99 | p99.9 | max |
|---|---|---|---|---|---|
| Cached ring | 83 | 84-125 | 125 | 167-292 | 34-49 us |
| Ring | 166 | 167 | 209 | 459-542 | 35-62 us |
| Naive ring | 167 | 167 | 209 | 250-500 | 37-47 us |
| Mutex + condvar | 2,208-2,333 | 2,792-3,458 | 5,000-5,125 | 7,875-8,541 | 49 us - 17.5 ms |

- Timer ticks ~41.7 ns: all values are multiples of that; min is not meaningful
  (cached min 0 ns); max is scheduler noise (condvar 17.5 ms in run 1, 82 us and
  49 us in runs 2-3).
- Cached ring ~2x faster than ring at p50, ~1.7x at p99, 1.8x-3x at p99.9.
  Naive and ring are indistinguishable (unlike throughput).
- Condvar: 13x-14x slower than ring and 27x-28x slower than cached at p50
  (24x and 40x at p99).
- Prediction that was wrong: I expected cached ~= ring in ping-pong. The producer
  side benefits even with one message in flight, because its cached copy stays
  valid for 1024 pushes.

## Instruments counters (bench_trace)
Method: see "Build and test" above. Ping-pong: 10M round trips. Stream: 50M items.
Values divided by n. Instruments screenshots were read by hand (Indian grouping).

### Ping-pong, per round trip (both threads together; cycles per thread)
| | cycles | store misses | retired load misses | all load misses |
|---|---|---|---|---|
| Ring | 502 | 0.61 | 5.89 | 68.2 |
| Naive ring | 513 | 2.29 | 9.82 | 68.2 |
| Cached ring | 262 | 0.06 | 6.13 | 65.3 |

Per thread (initiator / echo): retired load misses ring 2.95 / 2.95, naive
5.26 / 4.56, cached 4.09 / 2.04; store misses ring 0.23 / 0.37, naive
1.08 / 1.21, cached 0.04 / 0.02. Branch mispredicts ~0.98 per thread per round
trip in all runs (checks the divisor).
- Cycles halve with cached counters (matches the latency ratio).
- Store misses ~10x lower for cached than ring, ~3.8x higher for naive than ring.
- Load misses (retired and all) do NOT differ between ring and cached.
- Hypothesis (post hoc, untested): in the plain ring the producer reads the
  consumer's counter line, so the consumer's counter store must take the line
  back; in the cached ring it hits in cache.

### Stream, per item and thread (n = 50M)
| | thread | cycles | instr | retired load misses | store misses | mispredicts |
|---|---|---|---|---|---|---|
| Naive | producer | 115.1 | 14.1 | 0.048 | 1.08 | ~0 |
| Naive | consumer | 115.4 | 72.8 | 2.94 | 0.39 | 0.67 |
| Ring | producer | 58.8 | 13.0 | 0.97 | 0.14 | ~0 |
| Ring | consumer | 58.8 | 46.9 | 2.02 | 0.005 | 0.47 |
| Cached | producer | 25.6 | 10.8 | 0.001 | 0.14 | ~0 |
| Cached | consumer | 25.5 | 40.8 | 1.39 | 0.0005 | 0.10 |

- Producer retired load misses 0.97 -> 0.001 (~970x): ~one reload per 1024
  pushes (expected ~48,800 for 50M items, observed 50,116).
- Producer mispredicts ~0 in all runs: it never waits on a full queue, so it is the
  bottleneck (inference); cycles per item 115 -> 59 -> 26.
- Naive -> ring: producer store misses 1.08 -> 0.14 (padding). Ring -> cached:
  producer load misses ~970x, cycles 2.3x.
- Ratios here (1.96x, 2.3x) exceed the Google Benchmark ratios (1.34x, 1.75x at
  8 B): the harness does less work per item. Mechanism only, not speedup claims.

### Unexplained
- Naive producer: 0.05 retired load misses but 1.08 store misses per item.
- Ping-pong: echo thread has one fewer retired load miss per round trip with
  cached counters (2.95 -> 2.04), the initiator one more (2.95 -> 4.09).
- The first Instruments profile of bench_latency (timing inside the measured
  threads) gave retired load misses 30.8 (ring), 26.1 (naive), 9.0 (cached) per
  round trip, with the echo thread carrying most of them. Not reproduced in
  bench_trace; not used.

## Prototype results (before the rewrite)
One-slot-empty scheme, int payload, unbounded mutex queue. Kept for history only;
not comparable in absolute terms to the tables above.
- Mutex 9.6 M items/s, ring 35.9, ring + cached 54.8.
- Instruments, L1D load misses per item: mutex 0.32, ring 3.65, cached 1.77.
  Producer's load misses dropped ~157x with cached counters. Superseded by the
  stream measurement above (0.97 -> 0.001 per item, ~970x, final code).

## To do
- README.md (build, test, sanitize, benchmark, results tables, limitations).
- GitHub Actions on x86 Linux (build, ctest, TSan; benchmark only as a sanity check).
- Tag spsc-v1.0 when the README is done.
- Optional: producer-only-caching variant of the ring (isolates the mechanism),
  latency under load (paced producer), multi-word payload integrity test,
  batching, slot remap, 128 B payload test, emplace with raw storage.