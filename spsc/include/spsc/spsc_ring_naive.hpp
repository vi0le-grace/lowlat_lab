// spsc_ring_naive.hpp
//
// Naive lock-free SPSC ring buffer: exactly one producer thread calls
// try_push, exactly one consumer thread calls try_pop. Bounded to Capacity
// items (a power of two); all slots are usable. Neither function blocks.
//
// write_counter_ and read_counter_ are counters that only increase; the slot is
// (index & (Capacity - 1)). Empty: read == write. Full: write - read == Capacity.
// The item is written before the write_counter_ is updated, and read before
// read_counter_ is updated, so the other thread never sees a half-done slot.
//
// "Naive": default seq_cst ordering, no padding, no cached indices.


#pragma once

#include <atomic>
#include <cstddef>

namespace spsc {

template<typename T, std::size_t Capacity>
class SPSCRingBufferNaive {
private:
    static_assert(Capacity > 0, "Capacity must be greater than zero");
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");
    T buffer_[Capacity];
    std::atomic<std::size_t> read_counter_{0};   // how many items have been read in total
    std::atomic<std::size_t> write_counter_{0};  // how many items have been written in total
public:
    bool try_push(const T& item) {
        // check full
        std::size_t curr_write_counter = write_counter_.load();
        std::size_t curr_read_counter = read_counter_.load();

        if(curr_write_counter - curr_read_counter == Capacity) return false;

        std::size_t curr_write_index = (curr_write_counter & (Capacity-1));
        buffer_[curr_write_index] = item;

        // publish: the item is now visible to the consumer
        write_counter_.store(curr_write_counter + 1);
        return true;
    }
    bool try_pop(T& item) {
        std::size_t curr_write_counter = write_counter_.load();
        std::size_t curr_read_counter = read_counter_.load();

        if(curr_read_counter == curr_write_counter) return false;

        std::size_t curr_read_index = (curr_read_counter & (Capacity-1));
        item = buffer_[curr_read_index];

        // consume: the slot is now free for the producer
        read_counter_.store(curr_read_counter + 1);
        return true;
    }
};
}   // namespace spsc