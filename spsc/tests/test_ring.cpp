#include <iostream>
#include <cassert>
#include <memory>
#include <thread>

#include "spsc/spsc_ring.hpp"

using Queue4 = spsc::SPSCRingBuffer<int, 4>;
static_assert(alignof(Queue4) == 128);
static_assert(sizeof(Queue4) == 384);

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

void test_stress_two_threads() {
    constexpr int N = 5'000'000;

    auto q = std::make_unique<spsc::SPSCRingBuffer<int, 1024>>();
    int received = 0;

    std::thread consumer([&] {
        int item;
        for(int i = 0; i < N; i++) {
            while(!q->try_pop(item)) {}
            assert(item == i);
            received++;
        }
    });

    for(int i = 0; i < N; i++) {
        while(!q->try_push(i)) {}
    }

    consumer.join();
    assert(received == N);
}

int main() {

    test_empty();
    test_fill_and_full();
    test_order();
    test_wraparound();
    test_full_free_one_full_again();
    test_stress_two_threads();
    std::cout << "all tests passed\n";
    return 0;
}