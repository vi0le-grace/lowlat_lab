// mutex_queue.hpp
// 
// Baseline: std::queue guarded by a std::mutex
//
// Bounded to Capacity items. try_push returns false if full.
// try_pop returns false if empty. Neither blocks waiting for space or data.
// (Any thread may call either function; the mutex makes that safe)
//
// T must be default-constructible and copy- or move-assignable (slots are an
// array of live objects). A try_push that returns false does not consume its
// argument, so it can be retried.

#pragma once

#include <mutex>
#include <queue>
#include <cstddef>
#include <utility>

namespace spsc {

template<typename T, std::size_t Capacity>
class MutexQueue {
private:
    static_assert(Capacity > 0, "Capacity must be greater than zero");
    std::queue<T> queue_;
    std::mutex mutex_;
public:
    bool try_push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(queue_.size() >= Capacity) return false;
        queue_.push(item);
        return true;
    }
    bool try_push(T&& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(queue_.size() >= Capacity) return false;
        queue_.push(std::move(item));
        return true;
    }
    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop();
        return true;
    }
};
}   // namespace spsc