# spsc: single-producer single-consumer queues, measured

Header-only C++17 queues for one producer thread and one consumer thread, built as
a ladder where each rung changes one thing: a mutex + condition-variable baseline,
a naive lock-free ring (`seq_cst`, no padding), a ring with acquire/release
ordering and cache-line padding, and a ring with cached copies of the other
thread's counter. Each step is tested (unit tests, a two-thread stress test, and
ThreadSanitizer and AddressSanitizer builds), benchmarked for throughput and for
round-trip latency, and profiled with hardware counters. The design reasoning,
including what is a hypothesis and what is measured, is in
[docs/DESIGN.md](docs/DESIGN.md).

Everything here was measured on one Apple Silicon Mac (arm64, 8 cores, macOS) with
unpinned threads. Other machines are untested (an x86 Linux check is planned).

## Results

**Throughput** (1M items per iteration, capacity 1024, median of 10 repetitions,
million items per second; the time CV is in brackets):

| Queue | 8 B payload | 64 B payload |
|---|---|---|
| Mutex + condvar | 27.6 (81%) | 23.9 (5.1%) |
| Naive ring | 37.7 (2.7%) | 20.6 (3.3%) |
| Ring (acquire/release + padding) | 50.6 (0.2%) | 31.9 (2.4%) |
| Ring + cached counters | 88.6 (1.5%) | 93.7 (0.4%) |

The cached ring is about 3.2x (8 B) and 3.9x (64 B) the condvar queue. Run-to-run
spread is larger than the within-run CV: the cached ring at 8 B ranged from 79 to
90 M items/s across four runs, so treat ratios as approximate.

**Latency** (ping-pong round trip with one message in flight, 1M round trips,
three runs, ns; the clock ticks every ~42 ns, so values are coarse):

| Queue | p50 | p99 | p99.9 |
|---|---|---|---|
| Ring + cached counters | 83 | 125 | 167 to 292 |
| Ring | 166 | 209 | 459 to 542 |
| Naive ring | 167 | 209 | 250 to 500 |
| Mutex + condvar | 2,208 to 2,333 | 5,000 to 5,125 | 7,875 to 8,541 |

Round trip is about twice the one-way latency. Maximum values are dominated by
scheduler preemption and are not reported.

**What the counters show** (Instruments, stripped-down harness, 50M items): in the
plain ring the producer takes an L1D load miss on about 97% of its pushes (it
reads the line the consumer writes); with cached counters that drops to about
0.1%, one reload per 1,024 pushes, as designed. The cached ring's latency gain
shows up in store misses instead (about 10x fewer per round trip). Details and
the open questions are in section 12 of the design notes.

## What is measured, and what is not

- Throughput under a saturated stream, and latency with one message in flight.
  Latency under load and CPU cost are not measured. The rings spin, so a waiting
  thread burns a core; the condvar queue sleeps and is cheaper when idle.
- One machine, no thread pinning (macOS does not allow it), Apple clang, `-O2`.
- Counter values are not cache-line transfers, and several counter observations
  are unexplained (listed in the design notes).

## What is in here

| File | Queue |
|---|---|
| `include/spsc/mutex_condvar_queue.hpp` | `MutexCondvarQueue`: mutex + 2 condition variables, blocking `push`/`pop` |
| `include/spsc/mutex_queue.hpp` | `MutexQueue`: mutex + `std::queue`, `try_` interface (tested, not benchmarked: it starves in a retry loop) |
| `include/spsc/spsc_ring_naive.hpp` | `SPSCRingBufferNaive`: atomic counters, `seq_cst`, no padding |
| `include/spsc/spsc_ring.hpp` | `SPSCRingBuffer`: acquire/release, each counter on its own cache line |
| `include/spsc/spsc_ring_cached.hpp` | `SPSCRingBufferCached`: as above, plus a cached copy of the other thread's counter |
| `include/spsc/constants.hpp` | `CACHE_LINE_SIZE`: 128 on Apple Silicon, 64 on x86 |

Other folders: `tests/` (generic tests shared by the `try_` queues, a separate
test for the condvar queue), `bench/` (`bench_spsc` throughput, `bench_latency`
round trip, `bench_trace` for profiling with Instruments), `docs/` (design notes
and a working log), `results/` (raw benchmark output).

## Using a queue

```cpp
#include "spsc/spsc_ring_cached.hpp"

spsc::SPSCRingBufferCached<int, 1024> q;   // capacity must be a power of two

// producer thread only:
while (!q.try_push(42)) { /* full: spin, yield, or drop and count */ }

// consumer thread only:
int v;
if (q.try_pop(v)) { /* got an item */ }
```

- Exactly one thread calls `try_push` and exactly one other thread calls `try_pop`.
- Both functions are non-blocking and return `false` when the queue is full or empty.
  A failed `try_push` does not consume its argument.
- The queue stores its items inline, so a large queue should live on the heap or
  in static storage, not on a thread's stack.
- `T` must be default-constructible and copy- or move-assignable (move-only types
  such as `std::unique_ptr` work). There is no `emplace`.

## Build, test, benchmark

Requirements: CMake 3.20 or newer, a C++17 compiler, and Google Benchmark
(`brew install google-benchmark` on macOS). The CMake configure step looks for
Google Benchmark even if you only want the tests. Tested on macOS (Apple Silicon)
with Apple clang only.

From this folder:

```bash
cmake -S . -B build-cmake                  # default build type: Release (-O2 -DNDEBUG)
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

Benchmarks need a Release build, with the machine plugged in and other apps closed:

```bash
mkdir -p results
./build-cmake/bench_spsc --benchmark_out=results/run.json --benchmark_out_format=json
./build-cmake/bench_latency [all|ring|naive|cached|condvar]
```

Sanitizers use a separate build folder each (never take benchmark numbers from
them, and ThreadSanitizer and AddressSanitizer cannot be combined):

```bash
cmake -S . -B build-tsan -DSPSC_SANITIZER=thread       # or address
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

The tests keep `assert()` active even in a Release build, and the sanitizer builds
use a smaller stress size. `bench_trace` exists only to be profiled with
Instruments; how it was used is in `docs/notes.md`.

## Design notes

[docs/DESIGN.md](docs/DESIGN.md) covers the contract, the counter scheme, why
acquire/release is enough, why a stale cached counter is safe, padding and
false sharing, the break-it experiments (each memory-order weakening was caught by
ThreadSanitizer or a unit test), and the limits. [docs/notes.md](docs/notes.md) is
the working log with all measurements.