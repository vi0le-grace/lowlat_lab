#include <iostream>
#include <cassert>
#include <memory>
#include <thread>

#include "spsc/mutex_condvar_queue.hpp"

void test_basic() {
    spsc::MutexCondvarQueue<int, 4> q;
    for(int i = 1; i <= 4; i++) q.push(i * 10);
    int val;
    for(int i = 1; i <= 4; i++) {
        q.pop(val);
        assert(val == i * 10);
    }
}

template <typename Q>
void stress_two_threads(int n) {
    auto q = std::make_unique<Q>();
    int received = 0;

    std::thread consumer([&] {
        int item;
        for(int i = 0; i < n; i++) {
            q->pop(item);
            assert(item == i);
            received++;
        }
    });

    for(int i = 0; i < n; i++) q->push(i);

    consumer.join();
    assert(received == n);
}

int main() {
    constexpr int N = 1'000'000;
    test_basic();
    stress_two_threads<spsc::MutexCondvarQueue<int, 1>>(N / 10);     // both threads block constantly
    stress_two_threads<spsc::MutexCondvarQueue<int, 2>>(N / 10);
    stress_two_threads<spsc::MutexCondvarQueue<int, 1024>>(N);
    std::cout << "all tests passed\n";
    return 0;
}