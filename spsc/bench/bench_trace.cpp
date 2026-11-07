// bench_trace.cpp
//
// Workloads for profiling with Instruments (CPU Counters). Not a timing benchmark.
// main() only creates two worker threads and joins them, so the counters of each
// worker thread contain just the queue operations and the spin loops.
//
//   pingpong: thread A pushes i into `ping` and waits on `pong`; thread B echoes.
//             Same workload as bench_latency, minus all timing and recording.
//   stream:   one producer pushes 0..n-1, one consumer pops them (saturated queue).
//
// Usage: bench_trace <pingpong|stream> <ring|naive|cached> [n]
//   default n: 10,000,000 round trips (pingpong), 50,000,000 items (stream)

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "spsc/spsc_ring.hpp"
#include "spsc/spsc_ring_cached.hpp"
#include "spsc/spsc_ring_naive.hpp"

using U = std::uint64_t;

// ---- worker loops: named and not inlined, so Instruments shows them by name ----

template <typename Q>
__attribute__((noinline)) void pingpong_initiator(Q& ping, Q& pong, U rounds, U& checksum) {
    U sum = 0;
    for (U i = 0; i < rounds; ++i) {
        while (!ping.try_push(i)) {}
        U v;
        while (!pong.try_pop(v)) {}
        sum += v;
    }
    checksum = sum;
}

template <typename Q>
__attribute__((noinline)) void pingpong_echo(Q& ping, Q& pong, U rounds) {
    U v;
    for (U i = 0; i < rounds; ++i) {
        while (!ping.try_pop(v)) {}
        while (!pong.try_push(v)) {}
    }
}

template <typename Q>
__attribute__((noinline)) void stream_producer(Q& q, U n) {
    for (U i = 0; i < n; ++i) {
        while (!q.try_push(i)) {}
    }
}

template <typename Q>
__attribute__((noinline)) void stream_consumer(Q& q, U n, U& checksum) {
    U v;
    U sum = 0;
    for (U i = 0; i < n; ++i) {
        while (!q.try_pop(v)) {}
        sum += v;
    }
    checksum = sum;
}

// ---- driver ----

template <typename Q>
int run(const std::string& name, const std::string& mode, U n) {
    auto ping = std::make_unique<Q>();     // `ping` is the only queue used in stream mode
    auto pong = std::make_unique<Q>();
    U checksum = 0;

    auto t0 = std::chrono::steady_clock::now();    // main only: before the threads exist
    if (mode == "pingpong") {
        std::thread a([&] { pingpong_initiator(*ping, *pong, n, checksum); });
        std::thread b([&] { pingpong_echo(*ping, *pong, n); });
        a.join();
        b.join();
    } else {
        std::thread p([&] { stream_producer(*ping, n); });
        std::thread c([&] { stream_consumer(*ping, n, checksum); });
        p.join();
        c.join();
    }
    auto t1 = std::chrono::steady_clock::now();

    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::cout << name << " " << mode << " n=" << n << ": " << secs << " s, "
              << secs * 1e9 / static_cast<double>(n)
              << (mode == "pingpong" ? " ns per round trip" : " ns per item") << "\n";

    if (checksum != n * (n - 1) / 2) {
        std::cerr << "checksum mismatch: items lost or duplicated\n";
        return 1;
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: bench_trace <pingpong|stream> <ring|naive|cached> [n]\n";
        return 1;
    }
    std::string mode = argv[1];
    std::string queue = argv[2];
    if (mode != "pingpong" && mode != "stream") {
        std::cerr << "mode must be pingpong or stream\n";
        return 1;
    }
    U n = argc > 3 ? std::strtoull(argv[3], nullptr, 10)
                   : (mode == "pingpong" ? 10'000'000ULL : 50'000'000ULL);

    if (queue == "ring")   return run<spsc::SPSCRingBuffer<U, 1024>>("Ring", mode, n);
    if (queue == "naive")  return run<spsc::SPSCRingBufferNaive<U, 1024>>("NaiveRing", mode, n);
    if (queue == "cached") return run<spsc::SPSCRingBufferCached<U, 1024>>("CachedRing", mode, n);

    std::cerr << "queue must be ring, naive or cached\n";
    return 1;
}