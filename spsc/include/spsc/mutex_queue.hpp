// mutex_queue.hpp
// 
// Baseline: std::queue guarded by a std::mutex
//
// Bounded to Capacity items. try_push returns false if full.
// try_pop returns false if empty. Neither blocks waiting for space or data.
// (Any thread may call either function; the mutex makes that safe)

#pragma once

#include <mutex>
#include <queue>
#include <cstddef>

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
    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(queue_.empty()) return false;
        item = queue_.front();
        queue_.pop();
        return true;
    }
};
}   // namespace spsc