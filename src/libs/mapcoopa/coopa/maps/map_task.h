/**
 * @file map_task.h
 * @brief Completion, progress and cancellation for one asynchronous map operation.
 */

#ifndef COOPA_MAPS_MAP_TASK_H
#define COOPA_MAPS_MAP_TASK_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

#include <coopa/job/engine.h>
#include <coopa/job/handle.h>

namespace coopa {
namespace maps {

/**
 * @brief The `JobType` tag mapcoopa submits its work under.
 *
 * Above `coopa::job::k_max_job_types`, which opts out of thread dedication and
 * the diagnostics counters -- the same thing the animation and asset subsystems
 * do with their own tags. Map work is a bulk background job, not something the
 * frame loop schedules around.
 */
inline constexpr coopa::job::JobType k_map_job_type = 0x0A4D7000u;

/**
 * @struct MapTaskState
 * @brief The step counter an asynchronous map operation reports progress through.
 *
 * Held by `shared_ptr` on both sides. The job body captures a copy, so the
 * counter outlives the `MapTask` that handed it out and a body that is still
 * running after its task was destroyed writes to memory that is still there.
 */
struct MapTaskState {
    std::atomic<int> completed{0};   /**< @brief Steps finished so far. */
    std::atomic<int> total{1};       /**< @brief Steps the operation will take; never 0. */
    std::atomic<bool> finished{false}; /**< @brief Set on every exit path, cancelled or not. */

    /** @brief Marks one step done. Safe from any thread. */
    void step() { completed.fetch_add(1, std::memory_order_relaxed); }

    /** @brief Marks the whole operation over, however it ended. */
    void finish() { finished.store(true, std::memory_order_release); }
};

/**
 * @class MapTask
 * @brief A handle on one asynchronous map operation: poll it, measure it, abandon it.
 *
 * Modelled on `coopa::asset::AssetManager`'s poll-a-state shape rather than on
 * `std::future`: a game pumps this from its frame loop, and what it wants there
 * is "are you done yet, and how far along" without ever blocking.
 *
 * ```cpp
 * generator.set_job_engine(&engine);
 * MapTask task = generator.generate_async();
 * // ... each frame:
 * if (task.done()) { use(generator.graph()); } else { draw_bar(task.progress()); }
 * ```
 *
 * **Move-only, and the destructor cancels and waits.** An operation writes into
 * storage its caller owns -- the generator's graph, the exporter's images -- so a
 * task outliving the object it is filling in is a use-after-free. Blocking in the
 * destructor makes that unrepresentable, the same bargain `std::jthread` strikes.
 * A caller who wants the work to continue keeps the task alive.
 *
 * With no `JobEngine` injected the operation has already run to completion inline
 * by the time the task is constructed, and every method here answers accordingly.
 * That is deliberate: there is one API, one call pattern, and the serial path is
 * the default.
 */
class MapTask {
public:
    /** @brief Constructs an already-finished task with no work behind it. */
    MapTask() : state_(std::make_shared<MapTaskState>()) { state_->finish(); }

    /**
     * @brief Constructs a task for work that has already run inline.
     * @param state The step counter the work reported through.
     */
    explicit MapTask(std::shared_ptr<MapTaskState> state) : state_(std::move(state)) {
        if (!state_) {
            state_ = std::make_shared<MapTaskState>();
        }
        state_->finish();
    }

    /**
     * @brief Constructs a task for work in flight on an engine.
     * @param engine The engine running it; the task does not own it.
     * @param handle The group handle the work contributes to. The task owns this
     *               and closes it, so the caller must not close it as well.
     * @param state The step counter the work reports through.
     */
    MapTask(coopa::job::JobEngine* engine, coopa::job::JobHandle handle,
            std::shared_ptr<MapTaskState> state)
        : engine_(engine), handle_(handle), state_(std::move(state)) {
        if (!state_) {
            state_ = std::make_shared<MapTaskState>();
        }
    }

    MapTask(const MapTask&) = delete;
    MapTask& operator=(const MapTask&) = delete;

    MapTask(MapTask&& other) noexcept
        : engine_(other.engine_), handle_(other.handle_), state_(std::move(other.state_)) {
        other.engine_ = nullptr;
        other.handle_ = coopa::job::JobHandle{};
    }

    MapTask& operator=(MapTask&& other) noexcept {
        if (this != &other) {
            release_();
            engine_ = other.engine_;
            handle_ = other.handle_;
            state_ = std::move(other.state_);
            other.engine_ = nullptr;
            other.handle_ = coopa::job::JobHandle{};
        }
        return *this;
    }

    /** @brief Cancels the work and blocks until it has stopped. See the class note. */
    ~MapTask() { release_(); }

    /**
     * @brief Whether the work has stopped, whether it completed or was abandoned.
     *
     * Two sources, because neither alone covers every case: the shared flag is
     * set by the work itself on every exit path, and the handle reports complete
     * once its counter drains -- which also covers a job that never ran because
     * the engine shut down under it.
     *
     * @return True once nothing is still running.
     */
    bool done() const {
        return state_->finished.load(std::memory_order_acquire) || handle_.is_complete();
    }

    /**
     * @brief How much of the work is finished.
     * @return A fraction in `[0, 1]`, reaching exactly 1 once `done()`.
     */
    float progress() const {
        if (done()) {
            return 1.0f;
        }
        const int total = std::max(1, state_->total.load(std::memory_order_relaxed));
        const int completed = state_->completed.load(std::memory_order_relaxed);
        return std::clamp(static_cast<float>(completed) / static_cast<float>(total), 0.0f, 1.0f);
    }

    /**
     * @brief Asks the work to stop at its next checkpoint.
     *
     * Cooperative, and returns immediately. `JobEngine::cancel()` does not dequeue
     * anything -- the body still runs and checks the flag -- so what this bounds
     * is how much *more* work happens, not whether any does. Where those
     * checkpoints are, and therefore the worst-case latency, is documented on
     * whichever operation produced the task.
     */
    void cancel() {
        if (engine_ != nullptr && handle_.is_valid()) {
            engine_->cancel(handle_);
        }
    }

    /** @brief Whether `cancel()` has been called on this task. */
    bool cancelled() const { return handle_.is_cancelled(); }

    /**
     * @brief Blocks until `done()`.
     *
     * Safe to call from a job running on the same engine: `JobEngine::wait_for()`
     * has the caller participate as a real worker rather than idling, so waiting
     * on map work from inside other map work cannot deadlock.
     */
    void wait() {
        if (engine_ != nullptr && handle_.is_valid()) {
            engine_->wait_for(handle_);
        }
    }

private:
    /** @brief Cancels, waits, and closes the handle exactly once. */
    void release_() {
        if (engine_ != nullptr && handle_.is_valid()) {
            engine_->cancel(handle_);
            engine_->wait_for(handle_);
            handle_.close();
        }
        engine_ = nullptr;
        handle_ = coopa::job::JobHandle{};
    }

    /** @brief The engine running the work, or null when it ran inline. */
    coopa::job::JobEngine* engine_ = nullptr;
    /** @brief The group handle; default-constructed means "nothing in flight". */
    coopa::job::JobHandle handle_{};
    /** @brief Shared with the work itself, so it survives this task's destruction. */
    std::shared_ptr<MapTaskState> state_;
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_TASK_H
