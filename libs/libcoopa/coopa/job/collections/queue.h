/**
 * @file queue.h
 * @brief Thread-safe ParallelQueue collection template.
 */

#ifndef COOPA_JOB_COLLECTIONS_QUEUE_H
#define COOPA_JOB_COLLECTIONS_QUEUE_H

#include <vector>
#include <queue>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <map>
#include <string>

namespace coopa {
namespace job {

/**
 * @class ParallelQueue
 * @brief Thread-safe FIFO queue wrapper using std::queue, std::mutex, and std::condition_variable.
 * @tparam T Element type.
 */
template<typename T>
class ParallelQueue {
public:
    /**
     * @brief Pushes an item to the end of the queue.
     * @param item The element to push.
     */
    void push(T item) {
        { std::lock_guard<std::mutex> lock(m_mutex_); m_queue_.push(std::move(item)); }
        m_cv_.notify_one();
    }

    /**
     * @brief Tries to pop one item from the front of the queue.
     * @param item The item to be populated if successful.
     * @return True if an item was successfully popped, false if the queue was empty.
     */
    bool try_pop(T& item) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_queue_.empty()) {
            return false;
        }
        item = std::move(m_queue_.front());
        m_queue_.pop();
        return true;
    }

    /**
     * @brief Pops all elements from the queue and moves them into the provided vector.
     * @param items The destination vector.
     */
    void pop_all(std::vector<T>& items) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        while (!m_queue_.empty()) { items.push_back(std::move(m_queue_.front())); m_queue_.pop(); }
    }

    /**
     * @brief Checks if the queue is empty.
     * @return True if empty.
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_queue_.empty();
    }
    
    /**
     * @brief Returns the current size of the queue.
     * @return Number of elements in the queue.
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_queue_.size();
    }

    /**
     * @brief Clears all elements from the queue.
     */
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex_);
        std::queue<T> empty_queue;
        std::swap(m_queue_, empty_queue);
    }
private:
    std::queue<T> m_queue_; /**< Underlying standard queue container. */
    mutable std::mutex m_mutex_; /**< Mutex for thread safety. */
    std::condition_variable m_cv_; /**< Condition variable. */
};

}
}

#endif // COOPA_JOB_COLLECTIONS_QUEUE_H