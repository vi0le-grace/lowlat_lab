#ifdef NDEBUG
#error "These tests rely on assert(); do not compile them with -DNDEBUG"
#endif

#include <iostream>
#include <cassert>
#include <memory>
#include <thread>
#include <utility>

#include "spsc/mutex_condvar_queue.hpp"

#ifdef SPSC_STRESS_N
constexpr int N = SPSC_STRESS_N;
#else
constexpr int N = 1'000'000;
#endif

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

void test_move_only() {
    spsc::MutexCondvarQueue<std::unique_ptr<int>, 4> q;
    q.push(std::make_unique<int>(7));
    std::unique_ptr<int> out;
    q.pop(out);
    assert(out && *out == 7);
}

int main() {
    test_basic();
    test_move_only();
    stress_two_threads<spsc::MutexCondvarQueue<int, 1>>(N / 10);     // both threads block constantly
    stress_two_threads<spsc::MutexCondvarQueue<int, 2>>(N / 10);
    stress_two_threads<spsc::MutexCondvarQueue<int, 1024>>(N);
    std::cout << "all tests passed\n";
    return 0;
}