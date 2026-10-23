// test_queues.cpp
//
// Shared tests for the four try_push/try_pop queues: MutexQueue,
// SPSCRingBufferNaive, SPSCRingBuffer, SPSCRingBufferCached.
// (MutexCondvarQueue has a blocking push/pop interface and its own test file.)
//
// Each generic test takes the full queue type, e.g. SPSCRingBuffer<int, 4>.
// run_all<Queue> builds the capacities each test needs (1, 2, 4, 1024).
// Cached-only tests and layout checks for the padded rings are separate.
//
// Build (keep asserts on, so no -DNDEBUG):
//   clang++ -std=c++17 -O2 -Wall -Wextra -Iinclude tests/test_queues.cpp -o build/test_queues -pthread

#include <cassert>
#include <cstddef>
#include <iostream>
#include <memory>
#include <thread>

#include "spsc/mutex_queue.hpp"
#include "spsc/spsc_ring_naive.hpp"
#include "spsc/spsc_ring.hpp"
#include "spsc/spsc_ring_cached.hpp"

// ---- layout checks (padded rings only) ----
static_assert(alignof(spsc::SPSCRingBuffer<int, 4>) == 128);
static_assert(sizeof(spsc::SPSCRingBuffer<int, 4>) == 384);
static_assert(alignof(spsc::SPSCRingBufferCached<int, 4>) == 128);
static_assert(sizeof(spsc::SPSCRingBufferCached<int, 4>) == 384);

constexpr int STRESS_N = 5'000'000;

// ---- generic single-thread tests (Q has capacity 4 unless noted) ----

template <typename Q>
void test_empty() {
    Q q;
    int val;
    bool ok = q.try_pop(val);
    assert(!ok);
}

template <typename Q>
void test_fill_and_full() {
    Q q;
    bool ok;
    for(int i = 1; i <= 4; i++) {
        ok = q.try_push(i*10);
        assert(ok);
    }

    ok = q.try_push(50);
    assert(!ok);
}

template <typename Q>
void test_order() {
    Q q;
    bool ok;
    for(int i = 1; i <= 4; i++) {
        ok = q.try_push(i*10);
        assert(ok);
    }

    int val;
    for(int i = 1; i <= 4; i++) {
        ok = q.try_pop(val);
        assert(ok);
        assert(val == (i*10));
    }

    ok = q.try_pop(val);
    assert(!ok);
}

template <typename Q>
void test_wraparound() {
    Q q;
    bool ok;
    int val;

    for(int i = 0; i < 10; i++) {
        for(int j = 0; j < 3; j++) {
            ok = q.try_push(10*i + j);
            assert(ok);
        }

        for(int j = 0; j < 3; j++) {
            ok = q.try_pop(val);
            assert(ok);
            assert(val == (i*10 + j));
        }
    }
}

template <typename Q>
void test_full_free_one_full_again() {
    Q q;
    bool ok;
    int val;
    for(int i = 1; i <= 4; i++) {
        ok = q.try_push(10*i);
        assert(ok);
    }
    ok = q.try_pop(val);
    assert(ok);
    assert(val == 10);

    ok = q.try_push(50);
    assert(ok);

    for(int i = 2; i <= 5; i++) {
        ok = q.try_pop(val);
        assert(ok);
        assert(val == i*10);
    }
}

// One push, one pop, repeated.
template <typename Q>
void test_alternating() {
    Q q;
    bool ok;
    int val;
    for(int i = 0; i < 200; i++) {
        ok = q.try_push(i);
        assert(ok);
        ok = q.try_pop(val);
        assert(ok);
        assert(val == i);
    }
    ok = q.try_pop(val);
    assert(!ok);
}

// Q has capacity 1: full and empty are one step apart.
template <typename Q>
void test_capacity_one() {
    Q q;
    bool ok;
    int val;
    ok = q.try_pop(val);
    assert(!ok);
    for(int i = 0; i < 20; i++) {
        ok = q.try_push(i + 7);
        assert(ok);
        ok = q.try_push(-1);        // full
        assert(!ok);
        ok = q.try_pop(val);
        assert(ok);
        assert(val == i + 7);
        ok = q.try_pop(val);        // empty again
        assert(!ok);
    }
}

// ---- two-thread stress for any queue type ----

template <typename Q>
void stress_two_threads(int n) {
    auto q = std::make_unique<Q>();
    int received = 0;

    std::thread consumer([&] {
        int item;
        for(int i = 0; i < n; i++) {
            while(!q->try_pop(item)) {}
            assert(item == i);
            received++;
        }
    });

    for(int i = 0; i < n; i++) {
        while(!q->try_push(i)) {}
    }

    consumer.join();
    assert(received == n);
}

// ---- driver: runs every generic test on one queue family ----
// Queue is the queue template itself (e.g. spsc::SPSCRingBuffer); it is
// instantiated here with the capacities the tests need.

template <template <typename, std::size_t> class Queue>
void run_all(const char* name, int stress_n) {
    using Q1 = Queue<int, 1>;
    using Q2 = Queue<int, 2>;
    using Q4 = Queue<int, 4>;
    using Q1024 = Queue<int, 1024>;

    test_empty<Q4>();
    test_fill_and_full<Q4>();
    test_order<Q4>();
    test_wraparound<Q4>();
    test_full_free_one_full_again<Q4>();
    test_alternating<Q4>();
    test_capacity_one<Q1>();
    stress_two_threads<Q1024>(stress_n);        // normal-size ring
    stress_two_threads<Q2>(stress_n / 2);       // tiny ring: constant full/empty
    stress_two_threads<Q1>(stress_n / 4);       // capacity 1

    std::cout << "[ok] " << name << '\n';
}

// ---- cached-ring-only tests ----

using CachedQ4 = spsc::SPSCRingBufferCached<int, 4>;

// Producer's cached_read_counter_ goes stale: fill, drain, fill again.
// The second fill's first push sees "full" against the stale cache and must reload.
void test_producer_reload() {
    CachedQ4 q;
    bool ok;
    int val;
    for(int i = 0; i < 4; i++) {
        ok = q.try_push(i);
        assert(ok);
    }
    for(int i = 0; i < 4; i++) {
        ok = q.try_pop(val);
        assert(ok);
        assert(val == i);
    }
    for(int i = 0; i < 4; i++) {
        ok = q.try_push(100 + i);   // needs the reload to succeed
        assert(ok);
    }
    ok = q.try_push(999);           // really full: reload, re-check, fail
    assert(!ok);
    for(int i = 0; i < 4; i++) {
        ok = q.try_pop(val);
        assert(ok);
        assert(val == 100 + i);
    }
}

// Consumer's cached_write_counter_ goes stale: it must reload to see new items,
// and must still report empty when the reload finds nothing new.
void test_consumer_reload() {
    CachedQ4 q;
    bool ok;
    int val;
    ok = q.try_push(1);
    assert(ok);
    ok = q.try_pop(val);            // cache says empty, reload sees 1
    assert(ok);
    assert(val == 1);
    ok = q.try_pop(val);            // cache says empty, reload finds nothing new
    assert(!ok);
    ok = q.try_push(2);
    assert(ok);
    ok = q.try_pop(val);            // stale cache, must reload to see 2
    assert(ok);
    assert(val == 2);
    ok = q.try_pop(val);
    assert(!ok);
}

int main() {
    // The spin-retry mutex queue can starve one thread, so keep its stress small.
    run_all<spsc::MutexQueue>("MutexQueue", STRESS_N / 50);
    run_all<spsc::SPSCRingBufferNaive>("SPSCRingBufferNaive", STRESS_N);
    run_all<spsc::SPSCRingBuffer>("SPSCRingBuffer", STRESS_N);
    run_all<spsc::SPSCRingBufferCached>("SPSCRingBufferCached", STRESS_N);

    test_producer_reload();
    test_consumer_reload();
    std::cout << "[ok] cached-only tests\n";

    std::cout << "all tests passed\n";
    return 0;
}