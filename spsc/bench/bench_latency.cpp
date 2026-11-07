// bench_latency.cpp
//
// Round-trip latency: thread A sends a message through queue `ping`, thread B
// echoes it through queue `pong`. One message in flight at a time, so this
// measures the handoff cost, not queueing delay. One-way latency ~ RTT / 2.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <cstdlib>
#include <string>

#include "spsc/spsc_ring.hpp"
#include "spsc/spsc_ring_cached.hpp"
#include "spsc/spsc_ring_naive.hpp"
#include "spsc/mutex_condvar_queue.hpp"

using Clock = std::chrono::steady_clock;

void print_rtt(std::vector<std::uint64_t>& rtt_ns, const char* name) {
    std::sort(rtt_ns.begin(), rtt_ns.end());
    
    auto get_p = [&](double p) {
        std::size_t idx = static_cast<std::size_t>(p * rtt_ns.size());
        if (idx >= rtt_ns.size()) idx = rtt_ns.size() - 1; 
        return rtt_ns[idx];
    };

    std::cout << "--- " << name << " ---\n"
              << "Min:   " << rtt_ns.front() << " ns\n"
              << "p50:   " << get_p(0.50) << " ns\n"
              << "p90:   " << get_p(0.90) << " ns\n"
              << "p99:   " << get_p(0.99) << " ns\n"
              << "p99.9: " << get_p(0.999) << " ns\n"
              << "Max:   " << rtt_ns.back() << " ns\n\n";
}

template <typename Q>
void run_blocking(const char* name, int rounds, int warmup) {
    auto ping = std::make_unique<Q>();
    auto pong = std::make_unique<Q>();
    std::vector<std::uint64_t> rtt_ns;

    rtt_ns.reserve(rounds);

    std::thread echo([&] {
        for(int i = 0; i < warmup + rounds; i++) {
            std::uint64_t v;
            ping->pop(v);
            pong->push(v);
        }
    });

    for(int i = 0; i < warmup + rounds; i++) {
        auto t0 = Clock::now();

        std::uint64_t val = static_cast<std::uint64_t>(i);
        ping->push(val);
        std::uint64_t v;
        pong->pop(v);

        auto t1 = Clock::now();

        if(v != val) {
            std::cerr << "Mismatch error: expected " << val << ", got " << v << "\n";
            std::abort();
        }

        if(i >= warmup) {
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            rtt_ns.push_back(elapsed);
        }
    }

    echo.join();

    print_rtt(rtt_ns, name);
}

template <typename Q>
void run(const char* name, int rounds, int warmup) {
    auto ping = std::make_unique<Q>();
    auto pong = std::make_unique<Q>();
    std::vector<std::uint64_t> rtt_ns;

    rtt_ns.reserve(rounds);

    std::thread echo([&] {
        for(int i = 0; i < warmup + rounds; i++) {
            std::uint64_t v;
            while(!ping->try_pop(v)) {}
            while(!pong->try_push(v)) {}
        }
    });

    for(int i = 0; i < warmup + rounds; i++) {
        auto t0 = Clock::now();

        std::uint64_t val = static_cast<std::uint64_t>(i);
        while(!ping->try_push(val)) {}
        std::uint64_t v;
        while(!pong->try_pop(v)) {}

        auto t1 = Clock::now();

        if(v != val) {
            std::cerr << "Mismatch error: expected " << val << ", got " << v << "\n";
            std::abort();
        }

        if(i >= warmup) {
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
            rtt_ns.push_back(elapsed);
        }
    }

    echo.join();

    print_rtt(rtt_ns, name);
}

int main(int argc, char** argv) {
    std::string which = argc > 1 ? argv[1] : "all";
    bool ran = false;

    if(which == "all" || which == "ring") {
        run<spsc::SPSCRingBuffer<std::uint64_t, 1024>>("Ring", 1'000'000, 10'000);
        ran = true;
    }
    if(which == "all" || which == "naive") {
        run<spsc::SPSCRingBufferNaive<std::uint64_t, 1024>>("NaiveRing", 1'000'000, 10'000);
        ran = true;
    }
    if(which == "all" || which == "cached") {
        run<spsc::SPSCRingBufferCached<std::uint64_t, 1024>>("CachedRing", 1'000'000, 10'000);
        ran = true;
    }
    if(which == "all" || which == "condvar") {
        run_blocking<spsc::MutexCondvarQueue<std::uint64_t, 1024>>("MutexCondvar", 1'000'000, 10'000);
        ran = true;
    }

    if(!ran) {
        std::cerr << "usage: bench_latency [all|ring|naive|cached|condvar]\n";
        return 1;
    }
    return 0;
}