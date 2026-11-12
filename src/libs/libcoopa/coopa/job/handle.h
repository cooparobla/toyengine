/**
 * @file handle.h
 * @brief Defines JobHandle/CounterPool: generation-tagged, individually-reclaimed
 *        handles for tracking asynchronous job completion.
 *
 * Slots come from a lock-free free list and carry a generation tag, so handles
 * are allocated and released individually, at any time and independent of any
 * frame boundary, and a stale handle (one whose slot has since been recycled)
 * can always be told apart from a live one. Long-lived subsystems (Scene,
 * AssetManager) can therefore share a single engine without any of them needing
 * to own its frame boundary.
 *
 * A slot's lifetime tracks two independent things:
 *  - The **job counter**: how many submitted-but-incomplete jobs still reference
 *    this handle. is_complete() reads this, and it is what wakes dependency
 *    waiters (see dependency_graph.h) -- it must become zero the instant the
 *    tracked work finishes, regardless of whether anyone has "closed" the handle.
 *  - The **open** flag: whether the creator (whoever called create_handle() /
 *    submit_jobs()) has declared it is done with the handle, via close_handle()
 *    or a ScopedJobHandle's destructor. This exists solely to know when the slot
 *    is safe to recycle -- a handle that hits a job counter of zero may still
 *    receive more submissions later (the fan-in pattern: submit some jobs, let
 *    them finish, submit more against the very same handle), so counter-hits-zero
 *    alone is not sufficient license to hand the slot to someone else.
 *
 * A slot is only returned to the free list once BOTH are true -- job counter is
 * zero AND the handle has been closed -- decided via a single CAS-guarded
 * "reclaimed" flag so exactly one of the two racing paths (the completing job vs.
 * the closing caller) performs the reclaim. See CounterPool::close() and
 * CounterPool::try_reclaim_after_completion().
 */

#ifndef COOPA_JOB_HANDLE_H
#define COOPA_JOB_HANDLE_H

#include <atomic>
#include <cassert>
#include <cstdint>

namespace coopa {
namespace job {

/// @brief Sentinel value representing an uninitialized or invalid handle/slot index.
inline constexpr uint32_t k_invalid_handle_index = UINT32_MAX;

namespace detail {
/// @brief Storage whose address is the closed-waiter-stack sentinel. Never read.
inline char waiters_closed_tag = 0;
} // namespace detail

/**
 * @brief The value a slot's waiter stack holds once a completion has harvested it.
 *
 * Distinct from nullptr (open and empty) and from any real waiter address, so a
 * push can tell "nothing registered yet" apart from "this wave already
 * finished". dependency_graph.h owns the protocol; CounterPool only has to know
 * the value so add_jobs() can reopen a stack for a fresh wave of work.
 */
inline void* const k_waiters_closed = &detail::waiters_closed_tag;

/**
 * @class CounterPool
 * @brief Lock-free pool of generation-tagged counter slots.
 *
 * Slots are allocated from and returned to a Treiber-stack free list (a
 * tagged/packed {index, ABA-tag} head, so pop/push never confuses a slot that has
 * been popped-and-pushed-again with the one currently in hand). Each slot carries
 * its own generation counter, bumped on every allocation, so a JobHandle that
 * captured an older generation for the same index can always be told apart from
 * the slot's current occupant.
 */
class CounterPool {
public:
    /// @brief Result of allocate(): the slot index plus the generation stamped into it.
    struct Allocation {
        uint32_t index;      /**< Slot index, or k_invalid_handle_index if the pool is exhausted. */
        uint32_t generation; /**< Generation stamped into the slot at allocation time. */
    };

    /**
     * @brief Constructs a CounterPool with the given capacity, all slots free.
     * @param capacity Maximum number of concurrently in-flight handles.
     */
    explicit CounterPool(uint32_t capacity)
        : capacity_(capacity)
    {
        slots_ = new CounterSlot[capacity_];
        // Chain every slot into the free list: 0 -> 1 -> ... -> (capacity-1) -> invalid.
        // Single-threaded at construction time, so plain relaxed stores suffice.
        for (uint32_t i = 0; i < capacity_; ++i) {
            uint32_t next = (i + 1 < capacity_) ? (i + 1) : k_invalid_handle_index;
            slots_[i].next_free.store(next, std::memory_order_relaxed);
        }
        free_head_.store(pack(capacity_ > 0 ? 0 : k_invalid_handle_index, 0), std::memory_order_relaxed);
    }

    /// @brief Destructor. Frees the slot array.
    ~CounterPool() {
        delete[] slots_;
    }

    /// @brief Non-copyable.
    CounterPool(const CounterPool&) = delete;
    /// @brief Non-copyable.
    CounterPool& operator=(const CounterPool&) = delete;

    // --- Allocation / identity ---

    /**
     * @brief Pops a free slot, bumps its generation, and resets its state.
     * @return The new slot's {index, generation}, or {k_invalid_handle_index, 0}
     *         if the pool is exhausted.
     */
    Allocation allocate() {
        uint32_t idx = pop_free_();
        if (idx == k_invalid_handle_index) {
            return Allocation{k_invalid_handle_index, 0};
        }

        CounterSlot& slot = slots_[idx];
        slot.counter.store(0, std::memory_order_relaxed);
        slot.reclaimed.store(false, std::memory_order_relaxed);
        slot.cancelled.store(false, std::memory_order_relaxed);
        slot.waiters.store(nullptr, std::memory_order_relaxed);
        // Publish "open" last, after every other field is in its initial state, and
        // with release ordering -- close()/complete_job() acquire-load `open`
        // (indirectly, via the generation check ordering below) so they never
        // observe a half-reset slot.
        slot.open.store(true, std::memory_order_release);
        uint32_t gen = slot.generation.fetch_add(1, std::memory_order_acq_rel) + 1;

        outstanding_open_count_.fetch_add(1, std::memory_order_relaxed);
        return Allocation{idx, gen};
    }

    /**
     * @brief Checks whether `generation` is still the slot's current generation.
     *
     * A mismatch means the slot has since been reclaimed (and possibly
     * reallocated to a new handle) -- the handle that captured `generation` is
     * stale and must be treated as complete.
     */
    bool is_current(uint32_t index, uint32_t generation) const {
        if (index >= capacity_) return false;
        return slots_[index].generation.load(std::memory_order_acquire) == generation;
    }

    // --- Job counter (drives is_complete() and dependency-graph waiters) ---

    /**
     * @brief Adds `n` to the outstanding job count (called from submit()/submit_jobs()).
     *
     * A handle may be reused after its counter has already reached zero (the
     * fan-in pattern described in this file's doc). A completion harvest closes
     * the slot's waiter stack, so re-opening it here is what lets dependents
     * registered against this second wave actually wait for it instead of being
     * told the handle is already complete. Only ever CASes closed -> open, so a
     * live stack is never clobbered.
     */
    void add_jobs(uint32_t index, int32_t n) {
        int32_t before = slots_[index].counter.fetch_add(n, std::memory_order_release);
        if (before == 0) {
            void* expected = k_waiters_closed;
            slots_[index].waiters.compare_exchange_strong(
                expected, nullptr, std::memory_order_acq_rel, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Records one job's completion.
     * @return True if this call observed the counter transition to exactly zero --
     *         the caller must then fire any registered dependency waiters and
     *         attempt a reclaim via try_reclaim_after_completion().
     */
    bool complete_job(uint32_t index) {
        int32_t remaining = slots_[index].counter.fetch_sub(1, std::memory_order_acq_rel) - 1;
        return remaining == 0;
    }

    /// @brief Reads the current outstanding job count. is_complete() == (this == 0).
    int32_t load_counter(uint32_t index) const {
        return slots_[index].counter.load(std::memory_order_acquire);
    }

    // --- Cancellation ---

    /// @brief Marks every job still queued against this handle as cancelled.
    void cancel(uint32_t index) {
        slots_[index].cancelled.store(true, std::memory_order_release);
    }

    /// @brief Whether cancel() has been called for this slot's current occupant.
    bool is_cancelled(uint32_t index) const {
        return slots_[index].cancelled.load(std::memory_order_acquire);
    }

    // --- Waiters (type-erased; dependency_graph.h owns the WaiterNode type) ---

    /**
     * @brief Returns the slot's waiter-stack head for Treiber-stack push/pop.
     *
     * Stored as `void*` here so this header has no dependency on the job/
     * dependency-graph types built on top of it; dependency_graph.h reinterprets
     * this as a `std::atomic<WaiterNode*>`.
     */
    std::atomic<void*>& waiters_slot(uint32_t index) {
        return slots_[index].waiters;
    }

    // --- Lifecycle / reclaim ---

    /**
     * @brief Declares the creator done with this handle (called once, from
     *        JobHandle::close() / ScopedJobHandle's destructor).
     *
     * @param index Slot index.
     * @param generation Generation the caller's handle was allocated with.
     * @return True if this call must perform the reclaim (push the slot back onto
     *         the free list) -- i.e. the job counter was already zero. If false,
     *         either the handle was stale/already closed (no-op), or jobs are
     *         still outstanding and the eventual completion that drives the
     *         counter to zero will reclaim instead.
     */
    bool close(uint32_t index, uint32_t generation) {
        if (index >= capacity_) return false;
        CounterSlot& slot = slots_[index];

        if (slot.generation.load(std::memory_order_acquire) != generation) {
            return false; // Stale handle -- slot has already moved on.
        }

        bool expected_open = true;
        if (!slot.open.compare_exchange_strong(expected_open, false, std::memory_order_acq_rel)) {
            // Either already closed (double-close -- a caller bug) or the slot was
            // never open under this generation. Either way, not our job to reclaim.
            assert(false && "JobHandle closed more than once");
            return false;
        }
        outstanding_open_count_.fetch_sub(1, std::memory_order_relaxed);

        if (slot.counter.load(std::memory_order_acquire) != 0) {
            return false; // Jobs still outstanding; their completion will reclaim.
        }

        bool expected_reclaimed = false;
        return slot.reclaimed.compare_exchange_strong(expected_reclaimed, true, std::memory_order_acq_rel);
    }

    /**
     * @brief Attempts the reclaim after complete_job() reported the counter hit zero.
     * @return True if this call must perform the reclaim.
     */
    bool try_reclaim_after_completion(uint32_t index) {
        CounterSlot& slot = slots_[index];
        if (slot.open.load(std::memory_order_acquire)) {
            return false; // Not closed yet -- close() will reclaim later.
        }
        bool expected_reclaimed = false;
        return slot.reclaimed.compare_exchange_strong(expected_reclaimed, true, std::memory_order_acq_rel);
    }

    /**
     * @brief Pushes a slot back onto the free list.
     *
     * Must only be called once per allocation, by whichever of close() /
     * try_reclaim_after_completion() won the reclaim race. Defensively harvests
     * (and drops) any waiter stack still attached -- there should never be one
     * left by this point, since a well-behaved dependent re-checks is_complete()
     * immediately after registering (see dependency_graph.h), but this guarantees
     * no WaiterNode pointer is ever left dangling off a recycled slot.
     */
    void reclaim(uint32_t index) {
        slots_[index].waiters.store(nullptr, std::memory_order_release);
        push_free_(index);
    }

    /// @brief Total slot capacity.
    uint32_t capacity() const { return capacity_; }

    /// @brief Number of slots currently allocated-and-not-yet-closed (debug leak reporting).
    uint32_t debug_outstanding_count() const {
        return outstanding_open_count_.load(std::memory_order_relaxed);
    }

private:
    /**
     * @struct CounterSlot
     * @brief One handle's worth of tracking state.
     */
    struct CounterSlot {
        std::atomic<int32_t>  counter{0};     /**< Outstanding job count. */
        std::atomic<uint32_t> generation{0};  /**< Bumped on every allocate(). */
        std::atomic<bool>     open{false};    /**< False once close() has run. */
        std::atomic<bool>     reclaimed{false}; /**< CAS-guarded single-reclaim latch. */
        std::atomic<bool>     cancelled{false}; /**< Set by JobEngine::cancel(). */
        std::atomic<void*>    waiters{nullptr}; /**< Treiber stack of WaiterNode*, see dependency_graph.h. */
        std::atomic<uint32_t> next_free{0};   /**< Free-list link (index of next free slot). */
    };

    /// @brief Packs {index, tag} into a single 64-bit free-list head for ABA-safe CAS.
    static constexpr uint64_t pack(uint32_t index, uint32_t tag) {
        return (static_cast<uint64_t>(tag) << 32) | static_cast<uint64_t>(index);
    }
    static constexpr uint32_t unpack_index(uint64_t head) { return static_cast<uint32_t>(head & 0xFFFFFFFFu); }
    static constexpr uint32_t unpack_tag(uint64_t head)   { return static_cast<uint32_t>(head >> 32); }

    /// @brief Pops one slot index off the free list. Returns k_invalid_handle_index if empty.
    uint32_t pop_free_() {
        uint64_t old_head = free_head_.load(std::memory_order_acquire);
        for (;;) {
            uint32_t idx = unpack_index(old_head);
            if (idx == k_invalid_handle_index) return k_invalid_handle_index;

            uint32_t next = slots_[idx].next_free.load(std::memory_order_relaxed);
            uint64_t new_head = pack(next, unpack_tag(old_head) + 1);
            if (free_head_.compare_exchange_weak(old_head, new_head,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return idx;
            }
            // old_head was refreshed by the failed CAS; retry.
        }
    }

    /// @brief Pushes one slot index back onto the free list.
    void push_free_(uint32_t index) {
        uint64_t old_head = free_head_.load(std::memory_order_acquire);
        for (;;) {
            // next_free is only ever read by pop_free_() after it has already won
            // the CAS that published this slot as the new head, so a relaxed store
            // here is sufficient -- the CAS below carries the release needed to
            // make it visible.
            slots_[index].next_free.store(unpack_index(old_head), std::memory_order_relaxed);
            uint64_t new_head = pack(index, unpack_tag(old_head) + 1);
            if (free_head_.compare_exchange_weak(old_head, new_head,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return;
            }
        }
    }

    CounterSlot* slots_;               /**< Fixed-size array of counter slots. */
    uint32_t capacity_;                /**< Total slot count. */
    alignas(64) std::atomic<uint64_t> free_head_{0}; /**< Packed {index, ABA-tag} free-list head. */
    std::atomic<uint32_t> outstanding_open_count_{0}; /**< For debug-build leak reporting. */
};

/**
 * @class JobHandle
 * @brief Lightweight, trivially-copyable token for tracking asynchronous job completion.
 *
 * 16 bytes: a pool pointer, a slot index, and the generation stamped into that
 * slot at allocation time. Copy it around freely -- it carries no ownership by
 * itself. Ownership (the right, and responsibility, to eventually recycle the
 * underlying slot) belongs to whoever received it from create_handle() /
 * submit_jobs(), and must be given back exactly once via close() (or by wrapping
 * the handle in a ScopedJobHandle, which calls close() automatically). Forgetting
 * to close a handle leaks its slot -- CounterPool::debug_outstanding_count() and
 * JobEngine::shutdown()'s leak report exist to catch this.
 *
 * A handle whose slot has since been recycled (a "stale" handle) reports
 * is_complete() == true and never observes another handle's state -- see
 * CounterPool's class doc for the generation-tag mechanism that guarantees this.
 */
class JobHandle {
public:
    /// @brief Default constructor. Creates an invalid (always-complete) handle.
    JobHandle() : pool_(nullptr), index_(k_invalid_handle_index), generation_(0) {}

    /**
     * @brief Constructs a JobHandle referencing a specific slot.
     * @param pool Pointer to the owning CounterPool.
     * @param index The slot index within the pool.
     * @param generation The generation stamped into that slot at allocation time.
     */
    JobHandle(CounterPool* pool, uint32_t index, uint32_t generation)
        : pool_(pool), index_(index), generation_(generation) {}

    /// @brief Trivially destructible, copyable, assignable -- see class doc on ownership.
    ~JobHandle() = default;
    JobHandle(const JobHandle&) = default;
    JobHandle& operator=(const JobHandle&) = default;

    /**
     * @brief Checks if this handle references a valid, non-stale slot.
     * @return True if the handle has a valid pool/index and its generation still
     *         matches the slot's current occupant.
     */
    bool is_valid() const {
        return pool_ != nullptr && index_ != k_invalid_handle_index
            && pool_->is_current(index_, generation_);
    }

    /**
     * @brief Checks if all jobs contributing to this handle have finished.
     *
     * An invalid or stale handle always reports complete. The double
     * generation-check brackets the counter read to shrink (though, absent
     * hazard-pointer-style protection, not fully eliminate) the window in which a
     * concurrent close()+reclaim()+reallocate() of this exact slot could cause
     * this call to read a different occupant's counter -- see CounterPool's class
     * doc. In practice this only matters if a handle is retained well past when
     * it should have been treated as done.
     */
    bool is_complete() const {
        if (pool_ == nullptr || index_ == k_invalid_handle_index) return true;
        if (!pool_->is_current(index_, generation_)) return true;
        int32_t remaining = pool_->load_counter(index_);
        if (!pool_->is_current(index_, generation_)) return true;
        return remaining == 0;
    }

    /// @brief Whether JobEngine::cancel() has been called against this handle's current occupant.
    bool is_cancelled() const {
        if (!is_valid()) return false;
        return pool_->is_cancelled(index_);
    }

    /**
     * @brief Declares the caller done with this handle, allowing its slot to be
     *        recycled once any outstanding jobs finish. Safe to call at most once
     *        per handle returned from create_handle()/submit_jobs(); a second call
     *        is a no-op (asserted in debug builds). Prefer ScopedJobHandle, which
     *        calls this automatically and cannot be closed twice.
     */
    void close() const {
        if (pool_ == nullptr || index_ == k_invalid_handle_index) return;
        if (pool_->close(index_, generation_)) {
            pool_->reclaim(index_);
        }
    }

    /// @brief Returns the raw slot index for identity comparisons and dedup.
    uint32_t index() const { return index_; }

    /// @brief Returns the generation this handle was allocated with.
    uint32_t generation() const { return generation_; }

    /// @brief Returns the owning pool (used internally by dependency_graph.h/engine.h).
    CounterPool* pool() const { return pool_; }

    /// @brief Internal use only (engine.h): adds `n` to the outstanding job
    ///        count on submit(). No-op on an invalid/stale handle.
    void add_jobs_(int32_t n) const {
        if (is_valid()) pool_->add_jobs(index_, n);
    }

    /// @brief Equality by (pool, index, generation) identity.
    bool operator==(const JobHandle& other) const {
        return pool_ == other.pool_ && index_ == other.index_ && generation_ == other.generation_;
    }
    /// @brief Inequality.
    bool operator!=(const JobHandle& other) const { return !(*this == other); }
    /// @brief Less-than for sorting/dedup (by index, then generation).
    bool operator<(const JobHandle& other) const {
        if (index_ != other.index_) return index_ < other.index_;
        return generation_ < other.generation_;
    }

    /// @brief Internal use only (dependency_graph.h): called after this handle's job
    ///        completes, exactly once, to fire waiters and possibly reclaim.
    ///        Not part of the public contract -- exposed here rather than made a
    ///        free function so CounterPool's private layout never needs to be
    ///        shared outside this header.
    bool complete_job_and_check_zero_() const {
        // No generation check needed: a slot cannot be reclaimed while any
        // submitted-but-incomplete job still references it (close() only
        // reclaims once the job counter has already reached zero), so a Job
        // carrying this handle always names its own still-current slot. Only
        // guard against an invalid handle (e.g. the pool was exhausted when
        // this Job's handle was created).
        if (pool_ == nullptr || index_ == k_invalid_handle_index) return false;
        return pool_->complete_job(index_);
    }
    void try_reclaim_after_completion_() const {
        if (pool_ == nullptr || index_ == k_invalid_handle_index) return;
        if (pool_->try_reclaim_after_completion(index_)) {
            pool_->reclaim(index_);
        }
    }

private:
    CounterPool* pool_;    /**< Pointer to the owning counter pool (non-owning). */
    uint32_t index_;       /**< Slot index into the pool. */
    uint32_t generation_;  /**< Generation stamped into the slot at allocation time. */
};

/**
 * @class ScopedJobHandle
 * @brief RAII wrapper that closes its JobHandle exactly once, on destruction or reset().
 *
 * Move-only. Prefer this over a raw JobHandle whenever a handle's lifetime is
 * scoped to a block of code (the overwhelmingly common case: submit some jobs,
 * wait for them, done) -- it makes leaking a slot (forgetting to call close())
 * structurally impossible for that call site.
 */
class ScopedJobHandle {
public:
    ScopedJobHandle() = default;
    explicit ScopedJobHandle(JobHandle handle) : handle_(handle) {}

    ~ScopedJobHandle() { reset(); }

    ScopedJobHandle(const ScopedJobHandle&) = delete;
    ScopedJobHandle& operator=(const ScopedJobHandle&) = delete;

    ScopedJobHandle(ScopedJobHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = JobHandle();
    }
    ScopedJobHandle& operator=(ScopedJobHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = JobHandle();
        }
        return *this;
    }

    /// @brief Read-only access to the wrapped handle (for is_complete()/is_valid() etc).
    const JobHandle& get() const { return handle_; }
    /// @brief Implicit conversion so a ScopedJobHandle can be passed anywhere a JobHandle is expected.
    operator const JobHandle&() const { return handle_; }

    /// @brief Closes the current handle (if any) and releases ownership of it.
    void reset() {
        handle_.close();
        handle_ = JobHandle();
    }

private:
    JobHandle handle_;
};

} // namespace job
} // namespace coopa

#endif // COOPA_JOB_HANDLE_H
