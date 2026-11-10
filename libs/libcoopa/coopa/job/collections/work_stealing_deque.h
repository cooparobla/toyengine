/**
 * @file work_stealing_deque.h
 * @brief Lock-free Chase-Lev work-stealing deque for the job system.
 *
 * Implements the Chase-Lev dynamic circular work-stealing deque as described
 * in "Dynamic Circular Work-Stealing Deque" (Chase & Lev, 2005). The owner
 * thread pushes/pops from the bottom (no contention), while thieves steal
 * from the top using a single CAS operation.
 *
 * This implementation uses a fixed-capacity circular buffer (no dynamic
 * resizing) for deterministic memory behavior in realtime applications.
 *
 * A thief's steal() reserves a slot via a CAS on top_ and only *then* moves the
 * item out (moving first would race the other claimants on a non-trivial T).
 * Between the two it can be preempted, while the owner pushes a full lap and
 * wraps around onto that same slot -- so every slot carries a `busy` flag the
 * reader clears only after its move, and push() treats a still-busy slot as
 * full (its caller falls back to the global queue). Without it a preempted
 * thief could read a newer item, running one job twice and another never.
 */

#ifndef COOPA_JOB_WORK_STEALING_DEQUE_H
#define COOPA_JOB_WORK_STEALING_DEQUE_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility> // For std::move

#include <coopa/job/platform.h>

namespace coopa {
namespace job {

/**
 * @class WorkStealingDeque
 * @brief Lock-free single-producer multi-consumer work-stealing deque.
 *
 * @tparam T The element type. Must be move-constructible.
 *
 * Thread safety guarantees:
 * - push() and pop() may only be called by the **owner** thread.
 * - steal() may be called by **any** thread concurrently.
 * - push/pop and steal operate on opposite ends of the deque, minimizing contention.
 *
 * Memory ordering follows the Chase-Lev paper, expressed entirely through
 * ordered atomic operations rather than standalone atomic_thread_fence calls
 * (ThreadSanitizer does not model bare fences -- ordering must live on the
 * atomic operations themselves to be verifiable under TSan):
 * - bottom_: relaxed load/relaxed-or-seq_cst store from the owner (release
 *   on the common push() path; seq_cst on pop()'s contended path, matching
 *   the seq_cst top_ read it must not be reordered against), seq_cst/acquire
 *   load from thieves.
 * - top_: contended between owner pop() and thief steal() (CAS with seq_cst).
 */
template<typename T>
class WorkStealingDeque {
public:
    /// @brief Constructs a deque with the given fixed capacity (must be a power of 2).
    explicit WorkStealingDeque(std::size_t capacity = 1024)
        : capacity_(capacity),
          mask_(capacity - 1),
          buffer_(nullptr),
          bottom_(0),
          top_(0)
    {
        // Capacity must be a power of 2 for bitmask indexing.
        // If not, round up to the next power of 2.
        if ((capacity & (capacity - 1)) != 0) {
            std::size_t v = capacity;
            v--;
            v |= v >> 1;
            v |= v >> 2;
            v |= v >> 4;
            v |= v >> 8;
            v |= v >> 16;
            v |= v >> 32;
            v++;
            capacity_ = v;
            mask_ = v - 1;
        }

        buffer_ = new Slot[capacity_];
    }

    /// @brief Destructor. Frees the buffer.
    ~WorkStealingDeque() {
        delete[] buffer_;
    }

    /// @brief Non-copyable.
    WorkStealingDeque(const WorkStealingDeque&) = delete;

    /// @brief Non-copyable.
    WorkStealingDeque& operator=(const WorkStealingDeque&) = delete;

    /**
     * @brief Pushes an item onto the bottom of the deque (owner thread only).
     *
     * @param item The item to push (moved into the deque).
     * @return True if the item was successfully pushed, false if the deque is full.
     */
    bool push(T&& item) {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_acquire);

        // Check capacity.
        if (static_cast<std::size_t>(b - t) >= capacity_) {
            return false; // Full — fixed capacity, no resize.
        }

        // A thief that won this slot one lap ago may still be moving its item out (it reads
        // after its CAS, and can be preempted in between). Never overwrite it: report full,
        // and the caller falls back to its overflow path (JobEngine: the global queue).
        Slot& slot = buffer_[b & mask_];
        if (slot.busy.load(std::memory_order_acquire)) {
            return false;
        }
        slot.data = std::move(item);
        slot.busy.store(true, std::memory_order_relaxed);

        // Release store (not a relaxed store behind a separate release
        // fence): a thief's acquire load of bottom_ in steal() pairs
        // directly with this, giving the item write above a real
        // happens-before edge expressed entirely through atomic operations.
        // ThreadSanitizer does not model standalone atomic_thread_fence
        // calls (it warns "not supported with -fsanitize=thread" and
        // ignores them for its happens-before tracking), so a fence-based
        // push() would produce false-positive race reports on push()/steal()
        // despite being correctly synchronized per the C++ memory model --
        // expressing the ordering via the atomic operations themselves avoids
        // that blind spot, independent of any particular sanitizer's coverage.
        bottom_.store(b + 1, std::memory_order_release);

        return true;
    }

    /**
     * @brief Pops an item from the bottom of the deque (owner thread only).
     *
     * @param out_item The item popped (if successful).
     * @return True if an item was popped, false if the deque was empty.
     */
    bool pop(T& out_item) {
        int64_t b = bottom_.load(std::memory_order_relaxed) - 1;

        // Both seq_cst (not a relaxed store behind a separate seq_cst
        // fence): on weak memory models this store must not be reordered
        // past the top_ load below, and vice versa for a concurrent steal()
        // -- two seq_cst operations can never appear reordered relative to
        // each other in the single total order seq_cst guarantees. Standalone
        // atomic_thread_fence calls give the same guarantee per the C++
        // memory model, but ThreadSanitizer does not model them (see push()'s
        // doc), so expressing the ordering on the operations themselves
        // keeps this verifiable under TSan instead of reporting a false
        // positive.
        bottom_.store(b, std::memory_order_seq_cst);
        int64_t t = top_.load(std::memory_order_seq_cst);

        if (t < b) {
            // Non-empty, and not the last element. A concurrent steal() only
            // ever targets buffer_[top_ & mask_], which is a different slot
            // than buffer_[b & mask_] here, so moving out immediately is safe.
            out_item = std::move(buffer_[b & mask_].data);
            buffer_[b & mask_].busy.store(false, std::memory_order_relaxed);
            return true;
        } else if (t == b) {
            // Exactly one element left — a concurrent steal() may be racing
            // for this same slot. Resolve ownership via CAS BEFORE touching
            // buffer_[b & mask_]. Moving out first would race the thief's own
            // move-out of the identical slot whenever the CAS was going to
            // lose anyway; for a non-trivial T (e.g. Job, whose TaskWrapper
            // move nulls out the source's callable) that is a genuine data
            // race that can leave one side holding a corrupted, no-op value
            // while still reporting success.
            // `expected`, not `t`: a failed compare_exchange overwrites its expected argument
            // with the current top_ (t + 1, the thief's increment), and restoring bottom_ from
            // that would leave it at t + 2 -- one past the real end, resurrecting the slot just
            // consumed as a phantom element that is then popped or stolen a second time (a job
            // run twice, its handle's counter driven below zero).
            int64_t expected = t;
            bool won = top_.compare_exchange_strong(expected, t + 1,
                    std::memory_order_seq_cst, std::memory_order_relaxed);
            // Release, not relaxed: a thief that reads this bottom_ value must also see every
            // item the owner pushed before it. A relaxed store starts no release sequence, so a
            // thief acquiring it could read a stale slot (one job run twice, another never).
            bottom_.store(t + 1, std::memory_order_release);
            if (!won) {
                // Lost the race — thief took it. Do not touch the slot.
                return false;
            }
            out_item = std::move(buffer_[b & mask_].data);
            buffer_[b & mask_].busy.store(false, std::memory_order_relaxed);
            return true;
        } else {
            // Empty. Release for the same reason as the last-element path above.
            bottom_.store(t, std::memory_order_release);
            return false;
        }
    }

    /**
     * @brief Steals an item from the top of the deque (any thread).
     *
     * @param out_item The item stolen (if successful).
     * @return True if an item was stolen, false if the deque was empty or contended.
     */
    bool steal(T& out_item) {
        // Both seq_cst, for the same reason as pop()'s bottom_/top_ pair:
        // this thief's top_ and bottom_ reads must not be reordered relative
        // to each other on weak memory models, and a standalone fence would
        // give ThreadSanitizer no visibility into that ordering (see push()'s
        // doc). The seq_cst load of bottom_ still pairs with push()'s
        // release store to it for the item-write happens-before edge.
        int64_t t = top_.load(std::memory_order_seq_cst);
        int64_t b = bottom_.load(std::memory_order_seq_cst);

        if (t < b) {
            // Non-empty. Resolve ownership of this slot via CAS BEFORE
            // touching buffer_[t & mask_]: another thief racing for the same
            // slot (or the owner's pop(), see its own doc) is doing the
            // identical check. Moving the data out first would mean every
            // loser had performed an unsynchronized move-read of the same
            // slot the winner is also reading, which is a genuine data race
            // for a non-trivial T (Job's TaskWrapper move nulls out the
            // source, so a losing thief could hand back a corrupted, no-op
            // job while still reporting the winner's slot as claimed).
            if (!top_.compare_exchange_strong(t, t + 1,
                    std::memory_order_seq_cst, std::memory_order_relaxed)) {
                // Lost the race to another thief or to pop() — do not touch the slot.
                return false;
            }
            out_item = std::move(buffer_[t & mask_].data);
            // Release: the owner's push() acquires this before reusing the slot, so the move-out
            // above is complete before anything overwrites it.
            buffer_[t & mask_].busy.store(false, std::memory_order_release);
            return true;
        }

        return false; // Empty.
    }

    /**
     * @brief Returns the approximate number of items in the deque.
     *
     * This is a best-effort snapshot — the actual count may change immediately
     * after this call returns.
     *
     * @return Approximate item count.
     */
    std::size_t size_approx() const {
        int64_t b = bottom_.load(std::memory_order_relaxed);
        int64_t t = top_.load(std::memory_order_relaxed);
        return static_cast<std::size_t>(b >= t ? b - t : 0);
    }

    /**
     * @brief Checks if the deque is approximately empty.
     * @return True if no items are present (best-effort).
     */
    bool empty_approx() const {
        return size_approx() == 0;
    }

    /**
     * @brief Resets the deque to empty state. Owner thread only, not thread-safe.
     */
    void clear() {
        bottom_.store(0, std::memory_order_relaxed);
        top_.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Returns the fixed capacity of the deque.
     * @return Capacity in number of elements.
     */
    std::size_t capacity() const { return capacity_; }

private:
    /**
     * @struct Slot
     * @brief Cache-line-padded storage slot to prevent false sharing between
     *        adjacent elements accessed by different threads.
     */
    struct Slot {
        T data;
        /** @brief Holds an item not yet moved out. A thief clears it only after its read, so
         *         push() can tell a slot whose last thief is still reading -- see push(). */
        std::atomic<bool> busy{false};
    };

    std::size_t capacity_; /**< Fixed capacity (power of 2). */
    std::size_t mask_;     /**< Bitmask for circular indexing (capacity_ - 1). */
    Slot* buffer_;         /**< Dynamically allocated circular buffer. */

    /// @brief Bottom index (owner-side). Padded to its own cache line.
    alignas(k_cache_line_size) std::atomic<int64_t> bottom_;

    /// @brief Top index (thief-side). Padded to its own cache line.
    alignas(k_cache_line_size) std::atomic<int64_t> top_;
};

} // namespace job
} // namespace coopa

#endif // COOPA_JOB_WORK_STEALING_DEQUE_H
