#include <iostream>
#include <cassert>
#include <memory>
#include <thread>

#include "spsc/spsc_ring_cached.hpp"

using Queue4 = spsc::SPSCRingBufferCached<int, 4>;
static_assert(alignof(Queue4) == 128);
static_assert(sizeof(Queue4) == 384);

using Queue2 = spsc::SPSCRingBufferCached<int, 2>;
using Queue1 = spsc::SPSCRingBufferCached<int, 1>;
using Queue1024 = spsc::SPSCRingBufferCached<int, 1024>;

constexpr int STRESS_N = 5'000'000;

void test_empty() {
    Queue4 q;
    int val;
    bool ok = q.try_pop(val);
    assert(!ok);
}

void test_fill_and_full() {
    Queue4 q;
    bool ok;
    for(int i = 1; i <= 4; i++) {
        ok = q.try_push(i*10);
        assert(ok);
    }

    ok = q.try_push(50);
    assert(!ok);
}

void test_order() {
    Queue4 q;
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

void test_wraparound() {
    Queue4 q;
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

void test_full_free_one_full_again() {
    Queue4 q;
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

// Producer's cached_read_counter_ goes stale: fill, drain, fill again.
// The second fill's first push sees "full" against the stale cache and must reload.
void test_producer_reload() {
    Queue4 q;
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
    Queue4 q;
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

// One push, one pop, repeated: every call goes through a reload.
void test_alternating() {
    Queue4 q;
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

// Capacity 1: full and empty are one step apart.
void test_capacity_one() {
    Queue1 q;
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

// Two-thread stress for any queue type
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

int main() {

    test_empty();
    test_fill_and_full();
    test_order();
    test_wraparound();
    test_full_free_one_full_again();
    test_producer_reload();
    test_consumer_reload();
    test_alternating();
    test_capacity_one();
    stress_two_threads<Queue1024>(STRESS_N);    // normal-size ring
    stress_two_threads<Queue2>(STRESS_N / 2);   // tiny ring: constant full/empty
    stress_two_threads<Queue1>(STRESS_N / 4);   // capacity 1
    std::cout << "all tests passed\n";
    return 0;
}