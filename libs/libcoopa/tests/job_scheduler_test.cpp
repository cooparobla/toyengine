/**
 * @file job_scheduler_test.cpp
 * @brief coopa::job::JobScheduler: read/write component ordering, main-thread jobs, and the two
 *        end_frame() regressions (dropped main-thread jobs leaking pool slots, and stranding the
 *        jobs that depend on them).
 */
#include <coopa/testing/test.h>

#include <coopa/job/engine.h>
#include <coopa/job/scheduler.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <typeinfo>
#include <vector>

COOPA_TEST_SUITE("job_scheduler");

namespace {
struct ComponentA {};
} // namespace

COOPA_TEST(reader_of_a_component_runs_after_its_writer) {
    coopa::job::JobEngine engine(4);
    coopa::job::JobScheduler scheduler(engine);
    scheduler.begin_frame();

    std::atomic<int> step{0};
    // Writer of ComponentA; the sleep makes a reader that ignored the conflict observable.
    scheduler.add_job([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        step.store(1);
    }, 1, {}, {typeid(ComponentA)});
    // Reader of ComponentA.
    scheduler.add_job([&]() {
        if (step.load() == 1) step.store(2);
    }, 1, {typeid(ComponentA)}, {});

    auto handles = scheduler.submit_all();
    scheduler.wait_for_all(handles);
    EXPECT_EQ(step.load(), 2);

    scheduler.end_frame();
}

COOPA_TEST(main_thread_jobs_run_only_when_executed_by_the_caller) {
    coopa::job::JobEngine engine(4);
    coopa::job::JobScheduler scheduler(engine);
    scheduler.begin_frame();

    std::atomic<int> step{0};
    scheduler.add_job([&]() { step.store(42); }, 1, {}, {}, true /* is_main_thread_job */);

    auto handles = scheduler.submit_all();
    EXPECT_EQ(step.load(), 0);
    scheduler.execute_main_thread_jobs();
    EXPECT_EQ(step.load(), 42);

    scheduler.end_frame();
}

// A main-thread job that is queued but never executed must still have its
// handle slot returned to the CounterPool by end_frame(). The pool here is
// deliberately tiny so that a per-frame slot leak exhausts it long before the
// loop finishes, which is what makes this a real tripwire rather than a
// no-op: once the pool is exhausted, create_handle() starts handing back
// invalid handles and every subsequent submission silently degrades.
COOPA_TEST(end_frame_reclaims_unexecuted_main_thread_jobs) {
    constexpr uint32_t k_pool_capacity = 32;
    coopa::job::JobEngine engine(2, 256, k_pool_capacity);
    coopa::job::JobScheduler scheduler(engine);

    std::atomic<int> ran{0};

    for (uint32_t frame = 0; frame < k_pool_capacity * 8; ++frame) {
        scheduler.begin_frame();
        scheduler.add_job([&]() { ran.fetch_add(1); }, 1, {}, {}, true /* is_main_thread_job */);
        scheduler.submit_all();
        // Deliberately skip execute_main_thread_jobs() -- the dropped-job path.
        scheduler.end_frame();

        ASSERT_EQ(engine.get_counter_pool().debug_outstanding_count(), 0u);
    }

    // The dropped tasks must never have run...
    EXPECT_EQ(ran.load(), 0);
    // ...and the pool must still be able to serve a fresh handle.
    coopa::job::JobHandle handle = engine.create_handle();
    EXPECT_TRUE(handle.is_valid());
    handle.close();
}

// A job depending on a main-thread job that gets dropped by end_frame() must
// still be released, not stranded: end_frame() resolves the dropped handle's
// completion rather than merely closing it.
COOPA_TEST(dropped_main_thread_job_releases_its_dependents) {
    coopa::job::JobEngine engine(2);
    coopa::job::JobScheduler scheduler(engine);

    scheduler.begin_frame();
    scheduler.add_job([]() {}, 1, {}, {typeid(int)}, true /* is_main_thread_job */);
    std::vector<coopa::job::JobHandle> handles = scheduler.submit_all();
    ASSERT_EQ(handles.size(), 1u);

    std::atomic<bool> dependent_ran{false};
    coopa::job::JobHandle dependent = engine.create_handle();
    engine.submit([&]() { dependent_ran.store(true); }, 1, dependent, handles.data(), 1);

    scheduler.end_frame();

    engine.wait_for(dependent);
    EXPECT_TRUE(dependent_ran.load());
    dependent.close();
}
