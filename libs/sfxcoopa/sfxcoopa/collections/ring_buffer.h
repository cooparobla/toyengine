/**
 * @file ring_buffer.h
 * @brief Lock-free single-producer/single-consumer ring buffer.
 *
 * This is the whole cross-thread contract between the main thread and the
 * audio callback thread: RingBuffer<Command> carries control messages one
 * way, and RingBuffer<Event> carries completions the other way (the planned
 * streaming path would add a RingBuffer<float> of PCM; see plans/). No locks, no allocation after
 * construction, no exceptions on the hot path -- required on the audio
 * thread side, stated in every relevant @file block in this library.
 *
 * The atomics/padding idiom mirrors coopa::job::WorkStealingDeque
 * (coopa/job/collections/work_stealing_deque.h): signed, monotonically
 * increasing 64-bit indices that are never wrapped, masked only at buffer
 * access, with each index cache-line-padded via
 * alignas(coopa::job::k_cache_line_size) to prevent false sharing between
 * the producer and consumer. Unlike that deque (which is single-producer/
 * multi-consumer with a CAS-based steal path), this is strictly SPSC: push
 * side only ever touches head_, pop side only ever touches tail_.
 */

#ifndef SFXCOOPA_COLLECTIONS_RING_BUFFER_H
#define SFXCOOPA_COLLECTIONS_RING_BUFFER_H

#include <coopa/job/platform.h>

#include <atomic>
#include <cstdint>
#include <algorithm>

namespace coopa {
namespace sfx {
namespace collections {

/**
 * @class RingBuffer
 * @brief Lock-free SPSC circular buffer of T, capacity rounded up to a power of two.
 *
 * Thread-safety contract:
 * - push()/write() may only be called by the single producer thread.
 * - pop()/read() may only be called by the single consumer thread.
 * - available_read()/available_write() are safe to call from either thread
 *   for best-effort/diagnostic observation (the true value may change
 *   concurrently).
 * - clear() is owner-thread-only, not thread-safe against a concurrent
 *   push/pop from the other side.
 *
 * @tparam T Element type. Must be default-constructible and copyable.
 *
 * @code
 * coopa::sfx::collections::RingBuffer<float> ring(4096);
 * float samples[128];
 * uint32_t written = ring.write(samples, 128);   // producer thread
 * float out[128];
 * uint32_t read = ring.read(out, 128);            // consumer thread
 * @endcode
 */
template <typename T>
class RingBuffer {
public:
    /**
     * @brief Constructs a ring buffer able to hold at least `capacity` elements.
     * @param capacity Minimum capacity; rounded up to the next power of two.
     */
    explicit RingBuffer(uint32_t capacity)
        : capacity_(round_up_pow2_(capacity)),
          mask_(capacity_ - 1),
          buffer_(new T[capacity_]),
          head_(0),
          tail_(0) {}

    ~RingBuffer() { delete[] buffer_; }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;
    RingBuffer(RingBuffer&&) = delete;
    RingBuffer& operator=(RingBuffer&&) = delete;

    /**
     * @brief Pushes a single element. Producer-thread only.
     * @return false if the buffer is full (element not pushed).
     */
    bool push(const T& value) {
        const int64_t head = head_.load(std::memory_order_relaxed);
        const int64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= static_cast<int64_t>(capacity_)) return false;
        buffer_[static_cast<uint32_t>(head) & mask_] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Pops a single element. Consumer-thread only.
     * @return false if the buffer is empty (out left unmodified).
     */
    bool pop(T& out) {
        const int64_t tail = tail_.load(std::memory_order_relaxed);
        const int64_t head = head_.load(std::memory_order_acquire);
        if (tail == head) return false;
        out = buffer_[static_cast<uint32_t>(tail) & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Writes up to n elements from src. Producer-thread only.
     * @return The number of elements actually written (may be less than n if full).
     */
    uint32_t write(const T* src, uint32_t n) {
        const int64_t head = head_.load(std::memory_order_relaxed);
        const int64_t tail = tail_.load(std::memory_order_acquire);
        const uint32_t avail = capacity_ - static_cast<uint32_t>(head - tail);
        const uint32_t to_write = std::min(n, avail);
        for (uint32_t i = 0; i < to_write; ++i) {
            buffer_[(static_cast<uint32_t>(head) + i) & mask_] = src[i];
        }
        head_.store(head + to_write, std::memory_order_release);
        return to_write;
    }

    /**
     * @brief Reads up to n elements into dst. Consumer-thread only.
     * @return The number of elements actually read (may be less than n if empty).
     */
    uint32_t read(T* dst, uint32_t n) {
        const int64_t tail = tail_.load(std::memory_order_relaxed);
        const int64_t head = head_.load(std::memory_order_acquire);
        const uint32_t avail = static_cast<uint32_t>(head - tail);
        const uint32_t to_read = std::min(n, avail);
        for (uint32_t i = 0; i < to_read; ++i) {
            dst[i] = buffer_[(static_cast<uint32_t>(tail) + i) & mask_];
        }
        tail_.store(tail + to_read, std::memory_order_release);
        return to_read;
    }

    /** @brief Best-effort count of elements available to read. Safe from either thread. */
    uint32_t available_read() const {
        return static_cast<uint32_t>(head_.load(std::memory_order_acquire) -
                                      tail_.load(std::memory_order_acquire));
    }

    /** @brief Best-effort count of free slots available to write. Safe from either thread. */
    uint32_t available_write() const { return capacity_ - available_read(); }

    /** @brief Total capacity (rounded up to a power of two at construction). */
    uint32_t capacity() const { return capacity_; }

    /** @brief Drops all buffered elements. Owner thread only, not thread-safe. */
    void clear() { tail_.store(head_.load(std::memory_order_relaxed), std::memory_order_relaxed); }

private:
    static uint32_t round_up_pow2_(uint32_t v) {
        if (v < 2) return 2;
        v--;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v++;
        return v;
    }

    uint32_t capacity_; /**< Fixed capacity (power of 2). */
    uint32_t mask_;     /**< Bitmask for circular indexing (capacity_ - 1). */
    T* buffer_;         /**< Dynamically allocated circular buffer. */

    /// @brief Write index (producer-owned). Padded to its own cache line.
    alignas(coopa::job::k_cache_line_size) std::atomic<int64_t> head_;

    /// @brief Read index (consumer-owned). Padded to its own cache line.
    alignas(coopa::job::k_cache_line_size) std::atomic<int64_t> tail_;
};

}  // namespace collections
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_COLLECTIONS_RING_BUFFER_H
