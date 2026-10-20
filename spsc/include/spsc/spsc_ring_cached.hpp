// spsc_ring_cached.hpp
//
// Lock-free SPSC ring buffer with cached copies of the other thread's counter:
// exactly one producer thread calls try_push, exactly one consumer thread calls
// try_pop. Bounded to Capacity items (a power of two); all slots are usable.
// Neither function blocks.
//
// write_counter_ and read_counter_ are counters that only increase; the slot is
// (index & (Capacity - 1)). Empty: read == write. Full: write - read == Capacity.
//
// Ordering: same as SPSCRingBuffer. Each thread loads its own counter relaxed
// and the other thread's counter with acquire, and publishes its own counter
// with a release store after the slot has been written (push) or read (pop).
//
// Cached counters: the producer keeps cached_read_counter_ and the consumer
// keeps cached_write_counter_, plain variables that only their owner touches.
// Each side checks "full" / "empty" against its cached copy first, and reloads
// the other thread's counter (acquire) only when the queue looks full or empty,
// then re-checks. The other thread's counter only moves forward, so a stale copy
// can make the queue look fuller or emptier than it is, but never the reverse.
// A wrong "looks full/empty" costs one reload; it can never cause an overwrite
// or a read of an unwritten slot.
//
// Layout: each thread's counter and its private cached copy share one 128-byte
// cache line (Apple Silicon), so the two lines are not shared between threads
// and the cached copies add no cross-core traffic.
//
// Compared to SPSCRingBuffer: same ordering and padding, plus the cached
// counters, which avoid reading the other thread's cache line on most calls.

#pragma once

#include <atomic>
#include <cstddef>

namespace spsc {

template<typename T, std::size_t Capacity>
class SPSCRingBufferCached {
private:
    static_assert(Capacity > 0, "Capacity must be greater than zero");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    T buffer_[Capacity];
    // consumer-owned line
    alignas(128) std::atomic<std::size_t> read_counter_ {0};
    std::size_t cached_write_counter_ {0};  // consumer's private copy of write_counter_
    // producer-owned line
    alignas(128) std::atomic<std::size_t> write_counter_ {0};
    std::size_t cached_read_counter_ {0};   // producer's private copy of read_counter_
public:
    bool try_push(const T& item) {
        std::size_t curr_write_counter = write_counter_.load(std::memory_order_relaxed);

        if(curr_write_counter - cached_read_counter_ == Capacity) {
            cached_read_counter_ = read_counter_.load(std::memory_order_acquire);
            if(curr_write_counter - cached_read_counter_ == Capacity) return false;
        }

        std::size_t curr_write_index = (curr_write_counter & (Capacity - 1));
        buffer_[curr_write_index] = item;
        write_counter_.store(curr_write_counter + 1, std::memory_order_release);
        return true;
    }
    bool try_pop(T& item) {
        std::size_t curr_read_counter = read_counter_.load(std::memory_order_relaxed);

        if(curr_read_counter == cached_write_counter_) {
            cached_write_counter_ = write_counter_.load(std::memory_order_acquire);
            if(curr_read_counter == cached_write_counter_) return false;
        }

        std::size_t curr_read_index = (curr_read_counter & (Capacity - 1));
        item = buffer_[curr_read_index];
        read_counter_.store(curr_read_counter + 1, std::memory_order_release);
        return true;
    }
};
}   // namespace spsc