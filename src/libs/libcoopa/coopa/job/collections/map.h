/**
 * @file map.h
 * @brief Thread-safe ParallelMap collection template.
 */

#ifndef COOPA_JOB_COLLECTIONS_MAP_H
#define COOPA_JOB_COLLECTIONS_MAP_H

#include <parallel_hashmap/phmap.h> // Include the parallel_hashmap library

#include <memory>   // For std::shared_ptr, std::unique_ptr
#include <string>   // For std::string (example usage)
#include <iostream> // For print statements (example usage)
#include <optional> // For std::optional (safe return values)
#include <utility>  // For std::move, std::forward
#include <vector>   // For snapshot method

namespace coopa {
namespace job {

/**
 * @class ParallelMap
 * @brief A generic wrapper for phmap::parallel_flat_hash_map providing a simplified thread-safe interface.
 *
 * This class encapsulates the phmap::parallel_flat_hash_map, offering common
 * map operations with a focus on simplicity and leveraging the underlying
 * map's thread-safety for individual operations.
 *
 * Provided for consumers; the job system itself does not use it.
 *
 * @tparam KeyType The type of the keys in the map.
 * @tparam ValueType The type of the values in the map.
 * @tparam Hash The hash function for KeyType (defaults to std::hash<KeyType>).
 * @tparam Eq The equality predicate for KeyType (defaults to std::equal_to<KeyType>).
 * @tparam Allocator The allocator type (defaults to std::allocator<std::pair<const KeyType, ValueType>>).
 * @tparam N The number of shards for the parallel hash map (defaults to 8).
 * @tparam Mutex The mutex type for internal shard locking (defaults to std::shared_mutex).
 */
template <
    typename KeyType,
    typename ValueType,
    typename Hash = std::hash<KeyType>,
    typename Eq = std::equal_to<KeyType>,
    typename Allocator = std::allocator<std::pair<const KeyType, ValueType>>,
    size_t N = 8,
    typename Mutex = std::shared_mutex>
class ParallelMap
{
public:
    using MapType = phmap::parallel_flat_hash_map<
        KeyType,
        ValueType,
        Hash,
        Eq,
        Allocator,
        N,
        Mutex>; /**< Convenience type alias for underlying map. */

    using iterator = typename MapType::iterator; /**< Iterator type. */
    using const_iterator = typename MapType::const_iterator; /**< Const iterator type. */
    using value_type = typename MapType::value_type; /**< Map value pair type. */

    /**
     * @brief Default constructor.
     * The underlying phmap::parallel_flat_hash_map is initialized with its default constructor.
     */
    ParallelMap() = default;

    /**
     * @brief Destructor.
     * Cleans up the underlying phmap::parallel_flat_hash_map.
     */
    ~ParallelMap() = default;

    /**
     * @brief Adds or updates a key-value pair in the map.
     *
     * This method is thread-safe due to the internal locking mechanisms of
     * phmap::parallel_flat_hash_map. It uses lazy_emplace_l for efficient
     * insertion or in-place update if the key already exists.
     *
     * @param key The key to add or update.
     * @param value The value associated with the key.
     */
    void add(const KeyType& key, ValueType value)
    {
        map_.lazy_emplace_l(key,
                            [&](typename MapType::value_type& val) {
                                val.second = std::move(value);
                            },
                            [&](const typename MapType::constructor& ctor) {
                                ctor(key, std::move(value));
                            });
    }

    /**
     * @brief Retrieves the value associated with a given key.
     *
     * This method is thread-safe. It returns an std::optional containing the
     * value if the key is found, or an empty std::optional if the key is not present.
     *
     * @param key The key to search for.
     * @return An std::optional<ValueType> holding the value if found, else empty.
     */
    std::optional<ValueType> get(const KeyType& key) const
    {
        std::optional<ValueType> found_value;
        map_.if_contains(key, [&](const typename MapType::value_type& pair) {
            found_value = pair.second;
        });
        return found_value;
    }

    /**
     * @brief Removes a key-value pair from the map.
     *
     * This method is thread-safe.
     *
     * @param key The key of the element to remove.
     * @return true if the element was found and removed, false otherwise.
     */
    bool remove(const KeyType& key)
    {
        return map_.erase(key);
    }

    /**
     * @brief Checks if the map contains a specific key.
     *
     * This method is thread-safe.
     *
     * @param key The key to check for.
     * @return true if the key exists in the map, false otherwise.
     */
    bool contains(const KeyType& key) const
    {
        return map_.count(key) > 0;
    }

    /**
     * @brief Clears all elements from the map.
     */
    void clear() {
        map_.clear();
    }

    /**
     * @brief Returns the number of elements in the map.
     *
     * This method is thread-safe for phmap::parallel_flat_hash_map.
     *
     * @return The number of elements in the map.
     */
    size_t size() const
    {
        return map_.size();
    }

    /**
     * @brief Returns an iterator to the beginning of the map.
     *
     * @warning Iteration over phmap::parallel_flat_hash_map is "thread-safe"
     * in the sense that it won't crash due to concurrent modifications,
     * but it does not guarantee a globally consistent snapshot if other threads
     * are adding or removing elements during iteration. Elements added/removed
     * during iteration might or might not be included in the traversal.
     * For a globally consistent view, consider using the `snapshot()` method.
     *
     * @return An iterator to the first element.
     */
    iterator begin() {
        return map_.begin();
    }

    /**
     * @brief Returns a const iterator to the beginning of the map.
     * @warning See the warning for `begin()`.
     * @return A const iterator to the first element.
     */
    const_iterator begin() const {
        return map_.begin();
    }

    /**
     * @brief Returns an iterator to the end of the map.
     * @warning See the warning for `begin()`.
     * @return An iterator to the element past the last element.
     */
    iterator end() {
        return map_.end();
    }

    /**
     * @brief Returns a const iterator to the end of the map.
     * @warning See the warning for `begin()`.
     * @return A const iterator to the element past the last element.
     */
    const_iterator end() const {
        return map_.end();
    }

    /**
     * @brief Returns a const iterator to the beginning of the map.
     * @warning See the warning for `begin()`.
     * @return A const iterator to the first element.
     */
    const_iterator cbegin() const {
        return map_.cbegin();
    }

    /**
     * @brief Returns a const iterator to the end of the map.
     * @warning See the warning for `begin()`.
     * @return A const iterator to the element past the last element.
     */
    const_iterator cend() const {
        return map_.cend();
    }

    /**
     * @brief Creates and returns a snapshot of the map's contents.
     *
     * This method iterates over the map and copies all key-value pairs into
     * a std::vector. This provides a globally consistent view of the map
     * at the time the snapshot is taken, but it involves copying all elements.
     * This is useful when a consistent view is critical and concurrent
     * modifications during iteration are not acceptable.
     *
     * @return A std::vector containing copies of all key-value pairs in the map.
     */
    std::vector<value_type> snapshot() const
    {
        std::vector<value_type> snap;
        for (const auto& pair : map_) {
            snap.push_back(pair); // Copies the key-value pair
        }
        return snap;
    }

    /**
     * @brief Returns all keys currently stored in the map.
     * @return A std::vector of all keys.
     */
    std::vector<KeyType> keys() const
    {
        std::vector<KeyType> all_keys;
        for (const auto& pair : map_) {
            all_keys.push_back(pair.first);
        }
        return all_keys;
    }

private:
    MapType map_; /**< The underlying phmap::parallel_flat_hash_map instance. */
};

}
}

#endif // COOPA_JOB_COLLECTIONS_MAP_H
