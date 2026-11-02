// mutex_condvar_queue.hpp
//
// Baseline: std::queue guarded by a std::mutex, with two condition variables
// so a thread that cannot proceed sleeps instead of spinning on the lock.
//
// Bounded to Capacity items. push blocks while the queue is full; pop blocks
// while it is empty. (Any thread may call either function.)
// No shutdown mechanism: a thread blocked in push/pop stays blocked until the
// other side acts.
//
// T must be default-constructible and copy- or move-assignable (slots are an
// array of live objects).

#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

namespace spsc {

template<typename T, std::size_t Capacity>
class MutexCondvarQueue {
private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
public:
    void push(const T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] {return queue_.size() < Capacity; });
        queue_.push(item);
        lock.unlock();
        not_empty_.notify_one();
    }
    void push(T&& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [this] {return queue_.size() < Capacity; });
        queue_.push(std::move(item));
        lock.unlock();
        not_empty_.notify_one();
    }
    void pop(T& item) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_empty_.wait(lock, [this] {return !queue_.empty(); });
        item = std::move(queue_.front());
        queue_.pop();
        lock.unlock();
        not_full_.notify_one();
    }
};

}  // namespace spsc