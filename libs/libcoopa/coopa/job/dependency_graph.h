/**
 * @file dependency_graph.h
 * @brief Event-driven, unbounded-fan-in job dependency resolution.
 *
 * A job with dependencies becomes a PendingNode holding one WaiterNode per
 * dependency. Each WaiterNode is pushed onto the *dependency's own* slot (a
 * lock-free Treiber stack living in CounterPool -- see handle.h's
 * `waiters_slot()`). Whichever thread drives that dependency's job counter to
 * zero pops the whole stack and fires every node on it: no scanning, no global
 * lock, no polling, and no limit on how many dependencies a job may have.
 *
 * ### Exclusive ownership
 *
 * The design rests on one rule: **every WaiterNode is fired by exactly one
 * thread, which is the only thread that may dereference it.** That matters for
 * lifetime, not just for correctness of the count -- WaiterNodes live inside
 * their PendingNode, and the node is freed the moment its last dependency
 * fires. Any protocol that lets two threads reach the same WaiterNode lets one
 * of them free it under the other.
 *
 * Ownership is decided by a single atomic step. A harvest does not merely
 * empty a dependency's stack, it *closes* it (`exchange(k_waiters_closed)`), so:
 *
 * - `register_waiter_()` that pushes successfully hands the waiter to whoever
 *   harvests that stack. The submitter must never touch it again.
 * - `register_waiter_()` that is refused (the stack was already closed, or the
 *   handle is invalid/stale) means no harvester can ever see the waiter, so
 *   the submitter owns it and fires it itself.
 *
 * Exactly one of those holds per dependency, so no flag or CAS is needed to
 * arbitrate, and a node is only ever freed by the thread that fired its last
 * waiter.
 *
 * A dependency that completes between the push and the submitter's re-check is
 * covered by the same step: the submitter calls harvest_and_fire() itself, and
 * whichever of the two threads wins the `exchange` drains the stack while the
 * other sees it already closed and does nothing.
 *
 * A handle reused for a second wave of jobs (see handle.h's fan-in note) has
 * its stack reopened by CounterPool::add_jobs(), so dependents registered
 * against the new wave wait for it rather than seeing a permanently closed
 * stack.
 *
 * ### Known limit
 *
 * `waiters_slot()` is keyed by slot *index*, so a push that lands in the gap
 * between register_waiter_()'s generation check and its CAS can attach to a
 * slot that has just been recycled to a new generation. This is memory-safe --
 * the waiter is still unfired, so its node is still alive -- but that waiter is
 * then resolved on the new occupant's schedule rather than its own. The window
 * is the few instructions between the check and the CAS, and closing it
 * properly needs a generation tag packed into the stack head.
 */

#ifndef COOPA_JOB_DEPENDENCY_GRAPH_H
#define COOPA_JOB_DEPENDENCY_GRAPH_H

#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <coopa/job/context.h>
#include <coopa/job/handle.h>
#include <coopa/job/job.h>
#include <coopa/job/platform.h>

namespace coopa {
namespace job {

struct PendingNode;

/**
 * @struct WaiterNode
 * @brief One dependency edge: lives on the dependency handle's waiter stack
 *        until fired, at which point it decrements its owning PendingNode's
 *        unmet-dependency count exactly once.
 */
struct WaiterNode {
    PendingNode* node = nullptr;   /**< The job waiting on this dependency. */
    WaiterNode* next = nullptr;    /**< Next entry in the dependency's waiter stack. */
};

/**
 * @struct PendingNode
 * @brief A job parked until every one of its dependencies completes.
 *
 * Heap-allocated per dependency-bearing submission rather than drawn from a
 * fixed pool: unlike CounterPool, this path is not the
 * per-job hot path (the overwhelming majority of jobs have zero dependencies
 * and never construct a PendingNode at all), so the simplicity of ordinary
 * allocation outweighs pooling it. Owns inline storage for up to
 * k_max_inline_dependencies WaiterNodes (the common case) and spills to
 * separately-allocated nodes beyond that -- WaiterNode addresses must stay
 * stable once pushed onto a dependency's stack, which rules out a
 * reallocating container like std::vector<WaiterNode>.
 */
struct PendingNode {
    Job job;                                  /**< The job to run once unmet reaches 0. */
    std::atomic<int32_t> unmet{0};             /**< Dependencies not yet resolved. */
    WaiterNode inline_waiters[k_max_inline_dependencies];
    std::vector<std::unique_ptr<WaiterNode>> spill_waiters;
};

/**
 * @class DependencyGraph
 * @brief Owns the event-driven dependency resolution machinery for one JobEngine.
 */
class DependencyGraph {
public:
    /// @brief Invoked with a job the instant every one of its dependencies completes.
    using ReadySink = std::function<void(Job&&)>;

    explicit DependencyGraph(ReadySink sink) : on_ready_(std::move(sink)) {}

    /// @brief Non-copyable, non-movable (holds no state that would survive a move safely mid-flight).
    DependencyGraph(const DependencyGraph&) = delete;
    DependencyGraph& operator=(const DependencyGraph&) = delete;

    /**
     * @brief Parks `job` until every handle in `deps[0..dep_count)` completes.
     *
     * @param job The job to run once ready. Moved from.
     * @param deps Dependency handle array. May contain invalid/stale handles
     *             (treated as already complete).
     * @param dep_count Number of dependencies. No upper bound.
     */
    void submit_with_dependencies(Job&& job, const JobHandle* deps, uint32_t dep_count) {
        assert(dep_count > 0 && "submit_with_dependencies() called with no dependencies");

        PendingNode* node = new PendingNode();
        node->job = std::move(job);
        node->unmet.store(static_cast<int32_t>(dep_count), std::memory_order_relaxed);

        if (dep_count > k_max_inline_dependencies) {
            node->spill_waiters.reserve(dep_count - k_max_inline_dependencies);
            for (uint32_t i = k_max_inline_dependencies; i < dep_count; ++i) {
                node->spill_waiters.push_back(std::make_unique<WaiterNode>());
            }
        }

        // Waiter addresses are resolved up front, while `node` is still
        // guaranteed alive. From the first successful push onwards it may be
        // freed at any moment by the harvester that fires the last dependency,
        // so waiter_at_() (which reads node->spill_waiters) is not safe to call
        // again below.
        WaiterNode* inline_slots[k_max_inline_dependencies];
        std::vector<WaiterNode*> spill_slots;
        if (dep_count > k_max_inline_dependencies) spill_slots.reserve(dep_count - k_max_inline_dependencies);
        for (uint32_t i = 0; i < dep_count; ++i) {
            WaiterNode* w = waiter_at_(node, i);
            w->node = node;
            if (i < k_max_inline_dependencies) inline_slots[i] = w;
            else spill_slots.push_back(w);
        }
        auto waiter = [&](uint32_t i) {
            return i < k_max_inline_dependencies ? inline_slots[i] : spill_slots[i - k_max_inline_dependencies];
        };

        // Phase 1: try to hand every waiter to its dependency's harvester.
        // Anything refused here is ours to fire, and is collected rather than
        // fired immediately: firing drops `unmet`, and the fire that takes it
        // to zero frees the node, so nothing may read out of `node` afterwards.
        std::vector<WaiterNode*> ours;
        for (uint32_t i = 0; i < dep_count; ++i) {
            if (!register_waiter_(deps[i], waiter(i))) ours.push_back(waiter(i));
        }

        // Phase 2: a dependency that completed after we pushed still has to be
        // resolved, and its completer may already have decided there was
        // nothing on the stack to harvest. Harvesting it ourselves is safe and
        // idempotent -- whoever wins the exchange drains it, the other sees a
        // closed stack. This also covers a valid handle that never had any jobs
        // submitted against it, whose counter therefore never transitions to
        // zero and so never triggers a harvest of its own.
        for (uint32_t i = 0; i < dep_count; ++i) {
            if (deps[i].is_complete()) harvest_and_fire(deps[i]);
        }

        // Fired last, for the reason given above: the final one frees the node.
        for (WaiterNode* w : ours) fire_waiter_(w);
    }

    /**
     * @brief Called by JobEngine right after a handle's job counter hits zero:
     *        pops and fires every waiter registered against it.
     * @param completed The handle whose counter just reached zero.
     */
    void harvest_and_fire(const JobHandle& completed) {
        if (!completed.is_valid()) return;
        std::atomic<void*>& head_slot = completed.pool()->waiters_slot(completed.index());

        // Closing rather than merely emptying the stack is what makes the
        // popped waiters exclusively ours: no later push can land on it, and a
        // second harvest of the same generation gets the sentinel and stops.
        void* raw = head_slot.exchange(k_waiters_closed, std::memory_order_acq_rel);
        if (raw == k_waiters_closed) return;

        auto* w = static_cast<WaiterNode*>(raw);
        while (w) {
            // Read before firing: the fire may free the node this waiter lives in.
            WaiterNode* next = w->next;
            fire_waiter_(w);
            w = next;
        }
    }

private:
    static WaiterNode* waiter_at_(PendingNode* node, uint32_t i) {
        if (i < k_max_inline_dependencies) return &node->inline_waiters[i];
        return node->spill_waiters[i - k_max_inline_dependencies].get();
    }

    /**
     * @brief Tries to push `w` onto `dep`'s slot-local waiter stack.
     *
     * @return True if `w` was pushed, meaning the thread that harvests that
     *         stack now owns it and the caller must not touch it again. False
     *         if the handle is invalid/stale or its stack is already closed,
     *         meaning no harvester can reach `w` and the caller owns it.
     *
     * The generation is re-checked on every attempt rather than once up front,
     * to keep the recycled-slot window described in the @file doc as small as
     * the check-to-CAS gap.
     */
    static bool register_waiter_(const JobHandle& dep, WaiterNode* w) {
        if (!dep.is_valid()) return false;
        std::atomic<void*>& head_slot = dep.pool()->waiters_slot(dep.index());
        void* old_head = head_slot.load(std::memory_order_acquire);
        for (;;) {
            if (!dep.is_valid()) return false;
            if (old_head == k_waiters_closed) {
                // Either this handle is genuinely finished, or a concurrent
                // add_jobs() is mid-way through reopening the stack for a new
                // wave (it bumps the counter before reopening). Only the former
                // is a refusal; the latter is transient, so reload and retry.
                if (dep.is_complete()) return false;
                old_head = head_slot.load(std::memory_order_acquire);
                continue;
            }
            w->next = static_cast<WaiterNode*>(old_head);
            if (head_slot.compare_exchange_weak(old_head, w,
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return true;
            }
        }
    }

    /**
     * @brief Resolves one dependency edge, dispatching the job if it was the last.
     *
     * The caller must be `w`'s exclusive owner (see the @file doc). Deleting
     * the node here is therefore safe: no other thread can still be holding any
     * of its waiters.
     */
    void fire_waiter_(WaiterNode* w) {
        PendingNode* node = w->node;
        int32_t remaining = node->unmet.fetch_sub(1, std::memory_order_acq_rel) - 1;
        assert(remaining >= 0 && "WaiterNode fired more than once");
        if (remaining == 0) {
            Job ready_job = std::move(node->job);
            delete node;
            on_ready_(std::move(ready_job));
        }
    }

    ReadySink on_ready_;
};

} // namespace job
} // namespace coopa

#endif // COOPA_JOB_DEPENDENCY_GRAPH_H
