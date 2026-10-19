// spsc_ring.hpp
//
// Lock-free SPSC ring buffer: exactly one producer thread calls try_push,
// exactly one consumer thread calls try_pop. Bounded to Capacity items (a
// power of two); all slots are usable. Neither function blocks.
//
// write_counter_ and read_counter_ are counters that only increase; the slot is
// (index & (Capacity - 1)). Empty: read == write. Full: write - read == Capacity.
//
// Ordering: each thread loads its own counter with relaxed (it is the only
// writer) and the other thread's counter with acquire. It then publishes its
// own counter with a release store, after the slot has been written (push) or
// read (pop). A release store paired with the other thread's acquire load makes
// the slot's contents visible in the right order, so seq_cst is not needed.
//
// Padding: each counter sits on its own 128-byte cache line (Apple Silicon),
// so one thread's stores do not invalidate the line the other is using.
// 128 bytes is for Apple Silicon; x86 uses 64
//
// Compared to SPSCRingBufferNaive: acquire/release instead of seq_cst, plus
// padding.

#pragma once

#include <atomic>
#include <cstddef>

namespace spsc {

template<typename T, std::size_t Capacity>
class SPSCRingBuffer {
private:
    static_assert(Capacity > 0, "Capacity must be greater than zero");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    T buffer_[Capacity];
    alignas(128) std::atomic<std::size_t> read_counter_ {0};
    alignas(128) std::atomic<std::size_t> write_counter_ {0};
public:
    bool try_push(const T& item) {
        std::size_t curr_write_counter = write_counter_.load(std::memory_order_relaxed);
        std::size_t curr_read_counter = read_counter_.load(std::memory_order_acquire);

        if(curr_write_counter - curr_read_counter == Capacity) return false;

        std::size_t curr_write_index = (curr_write_counter & (Capacity - 1));
        buffer_[curr_write_index] = item;

        write_counter_.store(curr_write_counter + 1, std::memory_order_release);
        return true;
    }
    bool try_pop(T& item) {
        std::size_t curr_write_counter = write_counter_.load(std::memory_order_acquire);
        std::size_t curr_read_counter = read_counter_.load(std::memory_order_relaxed);

        if(curr_read_counter == curr_write_counter) return false;

        std::size_t curr_read_index = (curr_read_counter & (Capacity - 1));
        item = buffer_[curr_read_index];

        read_counter_.store(curr_read_counter + 1, std::memory_order_release);
        return true;
    }
};
}   // namespace spsc