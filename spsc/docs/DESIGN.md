# SPSC queue design

Design notes for the queues in `include/spsc/`. Measurements and raw data are in
`docs/notes.md` and `results/`; this document explains *why* the code looks the
way it does. Where something is an explanation I did not measure, it says so.

## 1. Contract

The ring buffers (`SPSCRingBufferNaive`, `SPSCRingBuffer`, `SPSCRingBufferCached`):

- **Exactly one producer thread** calls `try_push`, and **exactly one other
  consumer thread** calls `try_pop`. Nothing else is safe.
- **Bounded:** `Capacity` items, a power of two, and all `Capacity` slots are usable.
- **Non-blocking:** `try_push` returns `false` if the queue is full, `try_pop`
  returns `false` if it is empty. What to do then (spin, yield, drop and count)
  is the caller's decision.
- **A failed `try_push` does not consume its argument**, so `while (!q.try_push(std::move(x))) {}` is safe.
- **Requirements on `T`:** default-constructible and copy- or move-assignable
  (see section 8).

`MutexQueue` has the same `try_` interface but any thread may call either
function. `MutexCondvarQueue` is the blocking baseline: `push` sleeps while the
queue is full and `pop` sleeps while it is empty.

## 2. The ladder

Each rung changes one thing (the third rung changes two, see section 4).

| Queue | Synchronization | Ordering | Layout | Cached counters |
|---|---|---|---|---|
| `MutexCondvarQueue` | mutex + 2 condition variables, `std::queue` | n/a | n/a | n/a |
| `SPSCRingBufferNaive` | two atomic counters | `seq_cst` (default) | counters adjacent | no |
| `SPSCRingBuffer` | two atomic counters | acquire/release | each counter on its own cache line | no |
| `SPSCRingBufferCached` | two atomic counters | acquire/release | as above | yes |

`MutexQueue` (spin-retry, `try_` interface) is kept and tested but not
benchmarked: in a retry loop a failed attempt still takes the lock, so one
thread can starve the other (first benchmark: 21.6 ms median per 1M items but
a 2.2 s mean at 8 B and about 27 s per iteration at 64 B). The cause is likely
lock starvation but I did not verify it. The condvar queue replaced it as the
baseline.

## 3. Counters and the mask

There are two atomic counters. `write_counter_` is the total number of items ever
written; `read_counter_` is the total number ever read. They only increase. The
array slot is `counter & (Capacity - 1)`.

- **Empty:** `read == write`.
- **Full:** `write - read == Capacity`.

Why counters and not indices that wrap inside `[0, Capacity)`? With wrapped
indices, "full" and "empty" both look like `read == write` (with `Capacity` 4,
after 4 pushes the write index is back at 0, equal to the read index). The usual
fix is to leave one slot unused, so a ring of 1024 holds 1023. Counters that
keep growing tell the two states apart (`write - read` is 0 or `Capacity`), so
every slot is usable. My first prototype used the one-slot-empty scheme; the
final queues use counters.

**Why the counters can wrap safely.** In practice they never wrap: a 64-bit
counter at 100 million items per second takes about 5,800 years to overflow. The
argument still holds if one did, as long as three rules are kept:

1. The counters are **unsigned**. Unsigned overflow is defined (it wraps modulo
   2^64); signed overflow is undefined behaviour.
2. `Capacity` is a **power of two**, so it divides 2^64 and the mask sequence
   continues without a jump across the wrap.
3. Counters are only compared by **subtraction** (`write - read`), never with
   `<` or `>`. The difference stays correct across the wrap as long as the true
   difference is at most `Capacity`.

**Ownership.** Only the producer writes `write_counter_` and only the consumer
writes `read_counter_`. So each thread can read its own counter with a relaxed
load, because it is the only writer and always sees its own latest value.

## 4. Memory ordering

The ring buffer is a handoff protocol: the producer fills a slot and then
publishes it by increasing `write_counter_`; the consumer empties a slot and then
frees it by increasing `read_counter_`. The orderings make "fill, then publish"
and "empty, then free" visible to the other thread in that order.

| Operation | Order | Why |
|---|---|---|
| `try_push`: load `write_counter_` | relaxed | only the producer writes it |
| `try_push`: load `read_counter_` | acquire | pairs with the consumer's release store: the consumer's read of the slot is finished before the producer overwrites it |
| `try_push`: store `write_counter_` | release | publishes the slot: anyone who sees the new count also sees the slot's contents |
| `try_pop`: load `read_counter_` | relaxed | only the consumer writes it |
| `try_pop`: load `write_counter_` | acquire | pairs with the producer's release store: the slot's contents are visible before they are read |
| `try_pop`: store `read_counter_` | release | frees the slot only after the read of it is complete |

**Why not relaxed everywhere.** With relaxed operations the consumer could see
the new `write_counter_` before it sees the data written into the slot (the
compiler or the CPU may reorder them; Arm is weakly ordered), and then read a
stale slot. Each of the three release/acquire pairings above was weakened to
relaxed in turn, and ThreadSanitizer reported a data race every time, while the
ordinary unit tests did not notice (section 7).

**Why not the default `seq_cst`.** `seq_cst` adds a single global order over all
sequentially consistent operations. Two threads communicating through one pair
of variables with a one-way handoff do not need it; acquire/release gives exactly
the guarantee the protocol uses. In `SPSCRingBufferNaive` every operation,
including each thread's load of its own counter, is `seq_cst`; in
`SPSCRingBuffer` a thread's own counter is loaded relaxed.

**What the third rung's speedup is made of.** Naive to ring changes two things
at once: ordering and padding. The false-sharing demo (section 6) separates them
roughly. At 8 B: naive 37.7 M items/s, ring without padding 46.2 (about 1.23x
from ordering), ring with padding 50.6 (about 1.1x more from padding). At 64 B:
20.6, 29.3 (about 1.4x), 31.9 (about 1.07x). The ring-without-padding numbers
come from a different session than the naive numbers, so treat these ratios as
approximate. I did not investigate the ordering gain at the instruction level.

**Arm vs x86.** Everything here was measured on Apple Silicon (Arm64, weakly
ordered). On x86-64, acquire loads and release stores compile to plain moves,
because the hardware already orders them, while a `seq_cst` store needs a
full barrier. So I would expect the naive ring to lose by more on x86, and
I would expect ordering bugs to be harder to trigger at run time there (which
is why a race detector matters more). Neither is measured.

## 5. Cached counters

In `SPSCRingBuffer` every `try_push` reads `read_counter_` and every `try_pop`
reads `write_counter_`. That is the line the other core just wrote, so almost
every call pays a cross-core cache miss.

`SPSCRingBufferCached` keeps a private copy of the other thread's counter:
`cached_read_counter_` for the producer and `cached_write_counter_` for the
consumer. Each side checks "full" or "empty" against its copy first, and only if
that says full or empty does it reload the real counter (acquire) and check
again.

**Why a stale copy is always safe.** Both counters only increase, so a cached
copy can only be *behind* the real value:

- Producer: `cached_read <= read`, so `write - cached_read >= write - read`. The
  cache can claim "full" when there is room (the cost is one reload), but it can
  never claim "room" when the queue is full, so the producer never overwrites an
  unread slot.
- Consumer: `read <= cached_write <= write`. If `read != cached_write`, then
  `read < cached_write <= write`, so there really is an item. The cache can claim
  "empty" when there is data (one reload), but never claims "data" when the queue
  is empty.

The **re-check after the reload is essential**. Deleting it from `try_push` made
a push on a full queue succeed, which `test_fill_and_full` caught.

**Where the copies live.** Each copy sits on the same cache line as the counter
its own thread owns, so the copies add no cross-core traffic:

```
buffer_[Capacity]                              data slots
line:  read_counter_  | cached_write_counter_   consumer's line
line:  write_counter_ | cached_read_counter_    producer's line
```

**Result.** Ring to cached: 1.75x at 8 B and 2.94x at 64 B (median throughput,
2026-10-22). In an earlier Instruments run on a prototype of the same design
(one-slot-empty scheme, `int` payload, an older implementation), the producer's
L1D load misses dropped about 157x with cached counters. That measurement has
not been repeated on the final code.

## 6. Padding and false sharing

Each counter group starts on its own cache line (`alignas(CACHE_LINE_SIZE)`), so
one thread's stores do not invalidate the line the other thread is using.
`CACHE_LINE_SIZE` is in `constants.hpp`: 128 on Apple Silicon, 64 on x86-64. The
test file checks the layout with `static_assert` (`sizeof == 3 * line size` for
`Queue<int, 4>`).

False-sharing demo (2026-10-22, median M items/s; the unpadded build is the same
code with the alignment removed; padded runs bracket the unpadded one):

| | padded (a / b) | no padding | padded / no padding |
|---|---|---|---|
| Ring 8 B | 52.1 / 50.2 | 46.2 | 1.09x to 1.13x |
| Ring 64 B | 31.0 / 31.5 | 29.3 | 1.06x to 1.07x |
| Cached 8 B | 80.8 / 90.3 | 38.2 | 2.1x to 2.4x |
| Cached 64 B | 93.0 / 93.3 | 47.8 | 1.95x |

- Padding helps the plain ring a little, close to the noise at 8 B.
- Without padding the cached ring loses about half its throughput, and at 8 B
  it is no faster than the naive `seq_cst` ring.
- **Explanation (not measured):** the plain ring reads the other counter on every
  call, so that line bounces regardless of padding. The cached ring avoids those
  reads, but without padding each thread's own writes invalidate the line the
  other thread is using. Caching and padding work together: caching reduces how
  often you read the other side's line, padding stops your own writes from
  disturbing it.
- **Unexplained:** at 8 B the unpadded cached ring (38.2) is slower than the
  unpadded plain ring (46.2); at 64 B it is faster (47.8 vs 29.3).
- **Caveat:** the unpadded layout also places the counters directly after
  `buffer_`, so they may share a line with the last data slots.

The data slots themselves are not padded: with small items several slots share a
line, so when the queue is nearly empty the producer and consumer can touch the
same data line. A slot-remapping scheme or batching might help; neither has
been tried.

## 7. Testing

- **Single-thread tests**, run on all four `try_` queues: empty, fill and full,
  order, wraparound (counters well past the capacity), full-free-one-full-again,
  alternating push/pop, capacity 1.
- **Two-thread stress** with order checking at capacity 1024, 2 and 1. Tiny
  capacities force constant full/empty transitions.
- **Cached-ring tests** for the stale-cache paths: producer reload, consumer reload.
- **Move tests:** a counting type (a push of an rvalue must not copy), a
  move-only type (`std::unique_ptr<int>`), and a check that a failed push leaves
  its argument intact.
- **Layout:** `static_assert`s on alignment and size of the padded rings.
- **Sanitizers:** `-DSPSC_SANITIZER=thread` and `=address` build the tests with
  the sanitizer and a smaller stress size. Tests keep `assert()` active even in a
  Release build (`-UNDEBUG`), with an `#error` guard if `NDEBUG` is defined.

**Break-it experiments** (each change temporary, then reverted):

| Queue | Change | Caught by |
|---|---|---|
| Naive ring | `write_counter_.store` moved above the buffer write | TSan: data race on `buffer_` (the unit tests still passed) |
| Ring | each of the three release/acquire operations weakened to relaxed | TSan, all three |
| Cached ring | reload `acquire` weakened to `relaxed` | TSan (3 warnings); the unit tests did not notice |
| Cached ring | inner re-check deleted in `try_push` | `test_fill_and_full` |
| Cached ring | cached copy never updated | `test_wraparound` |

Logic bugs are caught by deterministic tests; ordering bugs are caught only by
a race detector.

**Limits of the testing.** TSan only reports races in executions that actually
happen, and this is not a formal proof of the ordering argument. The stress
tests use `int` payloads, and the benchmark checks only the sequence number
(`words[0]`) of 64-byte items, so a torn copy of a larger item would go
unnoticed. A multi-word payload where every word is derived from the sequence
number would close that gap.

## 8. Move semantics and requirements on `T`

Each queue has `try_push(const T&)` and `try_push(T&&)`, and `try_pop` moves the
item out of the slot. The full check comes before the move, so a failed push
leaves its argument untouched.

The rings store `T buffer_[Capacity]`, an array of live objects:

- `T` must be **default-constructible**, because all slots are constructed when
  the queue is built. A push does not construct anything; it assigns into an
  existing slot. `T` must also be **copy- or move-assignable**.
- A moved-from item stays alive in its slot until it is overwritten or the queue
  is destroyed. The array's own destructor cleans up whatever is left.
- There is no `emplace`. Constructing in place would need raw aligned storage,
  placement `new` and manual destruction in the queue's destructor. That is
  future work and would change the layout, so the benchmark would need a rerun.

The mutex queues store items in a `std::queue`, which constructs each element at
push time, so they do not need default construction.

## 9. Capacity that is not a power of two

The mask `counter & (Capacity - 1)` only maps counters onto slots correctly when
`Capacity` is a power of two, and the wraparound argument in section 3 also
depends on it. So the rings `static_assert` it.

Alternatives, none implemented:

- Use `counter % Capacity`. It works for any capacity and costs a division
  per operation (the compiler removes it for constants that are powers of two).
  The wrap of the 64-bit counter would then be a theoretical discontinuity.
- Keep indices wrapped inside `[0, Capacity)` and leave one slot unused to tell
  full from empty (the prototype scheme).
- Round the requested capacity up to the next power of two.

## 10. Benchmark method

1M items per iteration, one producer (main thread) and one consumer (spawned
thread), capacity 1024, 8 B and 64 B payloads, 10 repetitions, real time,
`-O2 -DNDEBUG`, threads not pinned (macOS does not allow it). Every iteration
checks that items arrive in order and complete, so a queue that drops or
reorders items fails instead of reporting a fast, wrong number.

Median throughput, 2026-10-22, million items per second (CV of time in brackets):

| Queue | 8 B | 64 B |
|---|---|---|
| Mutex + condvar | 27.6 (81%) | 23.9 (5.1%) |
| Naive ring | 37.7 (2.7%) | 20.6 (3.3%) |
| Ring (acq/rel + padding) | 50.6 (0.2%) | 31.9 (2.4%) |
| Ring + cached counters | 88.6 (1.5%) | 93.7 (0.4%) |

Speedup over the condvar queue: cached ring 3.2x (8 B) and 3.9x (64 B); naive
ring 1.36x and 0.86x. Run-to-run spread is larger than the within-run CV:
across four runs the cached ring at 8 B ranged from 79 to 90 M items/s, so
ratios are approximate. The condvar queue at 8 B is bimodal (CV 81%); its CPU
time is about 0.8x real time because its threads sleep, while the rings'
threads spin (1.0x).

## 11. Limits

- **Single producer, single consumer.** Multiple producers or consumers need a
  different design (for example per-slot sequence numbers).
- **Spinning:** the ring queues are designed to be polled. A waiting thread
  burns a core. The condvar baseline is cheaper when idle.
- **Throughput is measured; latency is not.** Under a saturated stream the rings
  win, but the p50/p99 per-message latency, which matters most for trading, and
  CPU cost are not measured.
- **Bounded size and no shutdown mechanism.**
- **One machine, Apple Silicon, unpinned threads.** x86 has not been measured.
  Only numbers measured on this machine are claimed.
- **Requirements on `T`** (section 8), and the testing limits (section 7).

Possible next steps: a latency benchmark with percentiles, batched publish
(several items per store), slot remapping, `emplace` with raw storage, and an
x86 CI run as a cross-check.