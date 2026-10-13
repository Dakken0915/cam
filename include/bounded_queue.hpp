#ifndef BOUNDED_QUEUE_H
#define BOUNDED_QUEUE_H

#include <queue>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <utility>
#include <stdexcept>
#include "frame.hpp"

template <typename T>
class BoundedQueue {
public:
    /**
     * @brief  Create a bounded queue with a positive capacity.
     * @param  [in] capacity Maximum number of queued items.
     * @note   Throws std::invalid_argument when capacity is zero.
     */
    explicit BoundedQueue(size_t capacity) : capacity_(capacity) {
        if (capacity == 0) throw std::invalid_argument("Queue capacity must be positive");
    }

    /**
     * @brief  Enqueue an item, dropping the oldest item when the queue is full.
     * @param  [in] item Value moved into the queue.
     * @note   Calls after close() are ignored; notifies a waiting consumer.
     */
    void push(T item) {
        /* No condition wait is needed here, so lock_guard is sufficient. */
        std::lock_guard<std::mutex> lock(mtx_);
        if (closed_) return;
        
        if(queue_.size() >= capacity_) {
            queue_.pop_front();
        }
        queue_.push_back(std::move(item));
        cv_.notify_one();
    }

    /**
     * @brief  Wait for an item or queue closure and dequeue the oldest item.
     * @param  [out] out Item moved from the queue when available.
     * @retval true An item was returned.
     * @retval false The queue is closed and empty.
     * @note   Items already queued remain available after close().
     */
    bool take(T& out) {
        std::unique_lock<std::mutex> lock(mtx_);
        /**
         * @brief  Resume the consumer only when an item is available or the queue is closed.
         */
        cv_.wait(lock, [&] { return !queue_.empty() || closed_; });

        if(queue_.empty())
            return false;

        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    /**
     * @brief  Close the queue and wake all waiting consumers.
     * @note   Safe to call repeatedly; rejects future pushes and cannot be reversed.
     */
    void close() {
        std::lock_guard<std::mutex> lock(mtx_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::deque<T>           queue_;
    size_t                  capacity_;
    bool                    closed_ = false;
};

#endif
