// bench_spsc.cpp
//
// Throughput benchmark: one producer (main thread), one consumer (spawned
// thread), a fixed number of items per iteration. Compares MutexCondvarQueue,
// SPSCRingBufferNaive, SPSCRingBuffer and SPSCRingBufferCached with 8-byte
// and 64-byte payloads.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "spsc/mutex_condvar_queue.hpp"
#include "spsc/spsc_ring_naive.hpp"
#include "spsc/spsc_ring.hpp"
#include "spsc/spsc_ring_cached.hpp"

template <std::size_t Bytes>
struct Payload {
    static_assert(Bytes >= 8 && Bytes % 8 == 0, "Payload size must be a multiple of 8, at least 8");
    std::uint64_t words[Bytes / 8];   // words[0] holds the sequence number
};

constexpr std::size_t kCapacity = 1024;
constexpr std::uint64_t kItems = 1'000'000;

template <typename Q, typename Item>
static void BM_Transfer(benchmark::State& state) {
    auto q = std::make_unique<Q>();   // created once; empty again after every iteration

    for (auto _ : state) {
        std::uint64_t errors = 0;

        std::thread consumer([&] {
            Item item{};
            for (std::uint64_t i = 0; i < kItems; ++i) {
                while (!q->try_pop(item)) {}
                errors += (item.words[0] != i);   // wrong or out-of-order item
            }
        });

        Item item{};
        for (std::uint64_t i = 0; i < kItems; ++i) {
            item.words[0] = i;
            while (!q->try_push(item)) {}
        }

        consumer.join();
        benchmark::DoNotOptimize(errors);
        if (errors != 0) state.SkipWithError("items lost, duplicated or reordered");
    }
    state.SetItemsProcessed(state.iterations() * kItems);
}

template <typename Q, typename Item>
static void BM_TransferBlocking(benchmark::State& state) {
    auto q = std::make_unique<Q>();

    for (auto _ : state) {
        std::uint64_t errors = 0;

        std::thread consumer([&] {
            Item item{};
            for (std::uint64_t i = 0; i < kItems; ++i) {
                q->pop(item);                       // sleeps while the queue is empty
                errors += (item.words[0] != i);
            }
        });

        Item item{};
        for (std::uint64_t i = 0; i < kItems; ++i) {
            item.words[0] = i;
            q->push(item);                          // sleeps while the queue is full
        }

        consumer.join();
        benchmark::DoNotOptimize(errors);
        if (errors != 0) state.SkipWithError("items lost, duplicated or reordered");
    }
    state.SetItemsProcessed(state.iterations() * kItems);
}

using P8 = Payload<8>;
using P64 = Payload<64>;

using Cv8 = spsc::MutexCondvarQueue<P8, kCapacity>;
using Naive8 = spsc::SPSCRingBufferNaive<P8, kCapacity>;
using Ring8 = spsc::SPSCRingBuffer<P8, kCapacity>;
using Cached8 = spsc::SPSCRingBufferCached<P8, kCapacity>;
using Cv64 = spsc::MutexCondvarQueue<P64, kCapacity>;
using Naive64 = spsc::SPSCRingBufferNaive<P64, kCapacity>;
using Ring64 = spsc::SPSCRingBuffer<P64, kCapacity>;
using Cached64 = spsc::SPSCRingBufferCached<P64, kCapacity>;

#define REGISTER(Q, Item, NAME)                      \
    BENCHMARK_TEMPLATE2(BM_Transfer, Q, Item)        \
        ->Name(NAME)                                 \
        ->UseRealTime()                              \
        ->Repetitions(10)                            \
        ->ReportAggregatesOnly(true)

#define REGISTER_BLOCKING(Q, Item, NAME)                 \
    BENCHMARK_TEMPLATE2(BM_TransferBlocking, Q, Item)    \
        ->Name(NAME)                                     \
        ->UseRealTime()                                  \
        ->Repetitions(10)                                \
        ->ReportAggregatesOnly(true)

REGISTER_BLOCKING(Cv8, P8, "MutexCondvar/8B");
REGISTER(Naive8, P8, "RingNaive/8B");
REGISTER(Ring8, P8, "Ring/8B");
REGISTER(Cached8, P8, "RingCached/8B");
REGISTER_BLOCKING(Cv64, P64, "MutexCondvar/64B");
REGISTER(Naive64, P64, "RingNaive/64B");
REGISTER(Ring64, P64, "Ring/64B");
REGISTER(Cached64, P64, "RingCached/64B");

BENCHMARK_MAIN();