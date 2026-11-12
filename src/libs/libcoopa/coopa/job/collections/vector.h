/**
 * @file vector.h
 * @brief Thread-safe ParallelVector collection template.
 */

#ifndef COOPA_JOB_COLLECTIONS_VECTOR_H
#define COOPA_JOB_COLLECTIONS_VECTOR_H

#include <vector>
#include <mutex>
#include <condition_variable>
#include <algorithm> // For std::remove_if, std::find, etc.

namespace coopa {
namespace job {

/**
 * @class ParallelVector
 * @brief Thread-safe vector wrapper using std::vector, std::mutex, and std::condition_variable.
 *
 * Provided for consumers; the job system itself does not use it.
 *
 * @tparam T Element type.
 */
template<typename T>
class ParallelVector {
public:
    /**
     * @brief Adds an item to the end of the vector.
     * @param item Item to add.
     */
    void push_back(T item) {
        {
            std::lock_guard<std::mutex> lock(m_mutex_);
            m_vector_.push_back(std::move(item));
        }
        m_cv_.notify_one(); // Notify waiting threads that an item has been added
    }

    /**
     * @brief Tries to pop the last item from the vector.
     * @param item Output parameter to hold popped item.
     * @return True if successful, false if empty.
     */
    bool try_pop_back(T& item) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_vector_.empty()) {
            return false;
        }
        item = std::move(m_vector_.back());
        m_vector_.pop_back();
        return true;
    }

    /**
     * @brief Tries to pop the first item from the vector.
     * @param item Output parameter to hold popped item.
     * @return True if successful, false if empty.
     */
    bool try_pop_front(T& item) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        if (m_vector_.empty()) {
            return false;
        }
        item = std::move(m_vector_.front());
        m_vector_.erase(m_vector_.begin()); // Erase the first element
        return true;
    }

    /**
     * @brief Atomically appends all items from a given vector to the end of this vector.
     * @param items The source vector containing elements to append (will be cleared).
     */
    void push_back_all(std::vector<T>& items) {
        {
            std::lock_guard<std::mutex> lock(m_mutex_);
            m_vector_.insert(m_vector_.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
            items.clear(); // Clear the source vector after moving elements
        }
        m_cv_.notify_all(); // Notify all waiting threads as multiple items might have been added
    }

    /**
     * @brief Removes all items from the vector and moves them into the provided vector.
     * @param items Destination vector to move elements to.
     */
    void pop_all(std::vector<T>& items) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        items.reserve(items.size() + m_vector_.size()); // Pre-allocate memory
        std::move(m_vector_.begin(), m_vector_.end(), std::back_inserter(items));
        m_vector_.clear();
    }

    /**
     * @brief Checks if the vector is empty.
     * @return True if empty.
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_vector_.empty();
    }

    /**
     * @brief Returns the current size of the vector.
     * @return Size of the vector.
     */
    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_vector_.size();
    }

    /**
     * @brief Clears all elements from the vector.
     */
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex_);
        m_vector_.clear();
    }

    /**
     * @brief Accesses an element at a specific index.
     * @param index Zero-based index to look up.
     * @return A copy of the element.
     * @throws std::out_of_range If index >= size.
     */
    T at(size_t index) const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        if (index >= m_vector_.size()) {
            throw std::out_of_range("ParallelVector::at index out of range");
        }
        return m_vector_.at(index);
    }

    /**
     * @brief Removes the first occurrence of a specific item from the vector.
     * @param item Item to look for.
     * @return True if the item was found and removed, false otherwise.
     */
    bool remove_one(const T& item) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        auto it = std::find(m_vector_.begin(), m_vector_.end(), item);
        if (it != m_vector_.end()) {
            m_vector_.erase(it);
            return true;
        }
        return false;
    }

    /**
     * @brief Removes all occurrences of a specific item from the vector.
     * @param item The value to match.
     * @return The number of items removed.
     */
    size_t remove_all(const T& item) {
        std::lock_guard<std::mutex> lock(m_mutex_);
        size_t initial_size = m_vector_.size();
        m_vector_.erase(std::remove(m_vector_.begin(), m_vector_.end(), item), m_vector_.end());
        return initial_size - m_vector_.size();
    }

    /**
     * @brief Returns a copy of the underlying vector.
     * @return Copy of the std::vector.
     */
    std::vector<T> get_copy() const {
        std::lock_guard<std::mutex> lock(m_mutex_);
        return m_vector_;
    }

private:
    std::vector<T> m_vector_; /**< Underlying standard vector container. */
    mutable std::mutex m_mutex_; /**< Mutex for thread safety. */
    std::condition_variable m_cv_; /**< Condition variable. */
};

}
}

#endif