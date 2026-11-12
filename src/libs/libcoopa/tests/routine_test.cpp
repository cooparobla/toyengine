/**
 * @file routine_test.cpp
 * @brief coopa::routine::RoutineRunner (C++20 coroutine "StartCoroutine"): resume timing for
 *        next_frame / seconds / frames / wait_until / wait_while, nested routines, stopping
 *        (including from inside the body) with locals unwound, starting from inside a tick,
 *        RoutineScope and handle lifetimes, waiting on JobHandles, on_worker() thread hops, and
 *        exceptions.
 *
 * RoutineSystem (scene phase, time scale, component helpers) is in routine_system_test.cpp.
 */
#include <coopa/testing/test.h>

#include <coopa/job/engine.h>
#include <coopa/job/handle.h>
#include <coopa/routine/routine.h>
#include <coopa/routine/runner.h>
#include <coopa/routine/yield.h>

#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

COOPA_TEST_SUITE("routine");

namespace cr = coopa::routine;

namespace {

/** @brief Shared scratch every routine body in this file writes through. */
struct Trace {
    std::vector<std::string> log;
    int  steps       = 0;
    bool flag        = false;
    bool reached_end = false;
};

/** @brief Counts one step per resume, suspending with next_frame() in between. */
cr::Routine count_frames(Trace* t, int n) {
    for (int i = 0; i < n; ++i) {
        ++t->steps;
        co_yield cr::next_frame();
    }
    t->reached_end = true;
}

/** @brief Marks reached_end after a seconds() wait. */
cr::Routine wait_seconds_then_mark(Trace* t, float duration) {
    co_yield cr::seconds(duration);
    t->reached_end = true;
}

/** @brief Waits a fixed number of ticks. */
cr::Routine wait_frames_then_mark(Trace* t, uint32_t count) {
    co_yield cr::frames(count);
    t->reached_end = true;
}

/** @brief Blocks on `flag` becoming true, then on it becoming false again. */
cr::Routine until_then_while(Trace* t) {
    co_yield cr::wait_until([t] { return t->flag; });
    t->log.push_back("until");
    co_yield cr::wait_while([t] { return t->flag; });
    t->log.push_back("while");
}

/** @brief Nested child routine; runs across two resumes. */
cr::Routine nested_child(Trace* t) {
    t->log.push_back("child-begin");
    co_yield cr::next_frame();
    t->log.push_back("child-end");
}

/** @brief Nests nested_child() and records around it. */
cr::Routine nested_parent(Trace* t) {
    t->log.push_back("parent-begin");
    co_yield nested_child(t);
    t->log.push_back("parent-end");
}

/** @brief Increments a counter when destroyed, proving a stopped routine unwinds. */
struct DestructorProbe {
    int* counter;
    explicit DestructorProbe(int* c) : counter(c) {}
    ~DestructorProbe() { ++(*counter); }
};

/** @brief Holds a live local across a suspension so stopping must destroy it. */
cr::Routine holds_a_local(Trace* t, int* destructions) {
    DestructorProbe probe(destructions);
    (void)probe;
    for (;;) {
        ++t->steps;
        co_yield cr::next_frame();
    }
}

/** @brief Everything a self-stopping routine needs to reach itself. */
struct SelfStop {
    cr::RoutineRunner* runner = nullptr;
    cr::RoutineHandle  handle;
    int  steps      = 0;
    bool after_stop = false;
};

/** @brief Stops itself from inside its own body, mid-resume. */
cr::Routine stops_itself(SelfStop* s) {
    ++s->steps;
    co_yield cr::next_frame();
    ++s->steps;
    s->runner->stop(s->handle);
    co_yield cr::next_frame();
    s->after_stop = true; // Must never run.
}

/** @brief Starts a second routine from inside its own body, mid-tick. */
cr::Routine starts_a_sibling(Trace* t, cr::RoutineRunner* runner) {
    t->log.push_back("outer-begin");
    co_yield cr::next_frame();
    runner->start(count_frames(t, 2));
    co_yield cr::next_frame();
    t->log.push_back("outer-end");
}

/** @brief Suspends on a JobHandle the caller completes by hand. */
cr::Routine awaits_job(Trace* t, coopa::job::JobHandle handle) {
    t->log.push_back("before");
    co_yield cr::wait_for(handle);
    t->log.push_back("after");
}

/** @brief Records which thread the worker body ran on, and which one resumed it. */
struct ThreadProbe {
    std::thread::id caller;
    std::thread::id worker;
    std::thread::id resumed;
    bool body_ran = false;
};

/** @brief Runs a body on the engine and resumes on the tick thread. */
cr::Routine hops_to_worker(ThreadProbe* p) {
    co_yield cr::on_worker([p] {
        p->worker = std::this_thread::get_id();
        p->body_ran = true;
    });
    p->resumed = std::this_thread::get_id();
}

/** @brief Throws out of the body after one suspension. */
cr::Routine throws_after_a_yield(Trace* t) {
    t->log.push_back("before-throw");
    co_yield cr::next_frame();
    throw std::runtime_error("routine blew up");
}

} // namespace

COOPA_TEST(body_runs_to_first_yield_on_start_then_resumes_once_per_tick) {
    cr::RoutineRunner runner;
    Trace t;
    auto handle = runner.start(count_frames(&t, 3));

    // Unity's StartCoroutine parity: everything before the first co_yield has
    // already run by the time start() returns.
    EXPECT_EQ(t.steps, 1);
    EXPECT_TRUE(handle.is_running());
    EXPECT_EQ(runner.active_count(), 1u);

    runner.tick(0.016f);
    EXPECT_EQ(t.steps, 2);
    runner.tick(0.016f);
    EXPECT_EQ(t.steps, 3);
    EXPECT_FALSE(t.reached_end);

    runner.tick(0.016f); // The loop exits and the body runs to completion.
    EXPECT_TRUE(t.reached_end);
    EXPECT_FALSE(handle.is_running());
    EXPECT_EQ(runner.active_count(), 0u);
}

COOPA_TEST(seconds_wait_resumes_on_the_exact_tick_despite_float_drift) {
    cr::RoutineRunner runner;
    Trace t;
    runner.start(wait_seconds_then_mark(&t, 1.0f));

    // 0.1f is not exactly representable, so ten subtractions accumulate error;
    // the routine must still resume on the tenth tick, not the eleventh.
    for (int i = 0; i < 9; ++i) {
        runner.tick(0.1f);
        ASSERT_FALSE(t.reached_end);
    }
    runner.tick(0.1f);
    EXPECT_TRUE(t.reached_end);
}

COOPA_TEST(frames_wait_counts_ticks) {
    cr::RoutineRunner runner;
    Trace three;
    Trace one;
    runner.start(wait_frames_then_mark(&three, 3));
    runner.start(wait_frames_then_mark(&one, 1));

    runner.tick(0.016f);
    EXPECT_TRUE(one.reached_end); // frames(1) is next_frame()
    EXPECT_FALSE(three.reached_end);
    runner.tick(0.016f);
    EXPECT_FALSE(three.reached_end);
    runner.tick(0.016f);
    EXPECT_TRUE(three.reached_end);
}

COOPA_TEST(wait_until_and_wait_while_poll_their_predicate_each_tick) {
    cr::RoutineRunner runner;
    Trace t;
    runner.start(until_then_while(&t));

    runner.tick(0.016f);
    runner.tick(0.016f);
    EXPECT_EQ(t.log.size(), 0u);

    t.flag = true;
    runner.tick(0.016f);
    ASSERT_EQ(t.log.size(), 1u);
    EXPECT_EQ(t.log[0], std::string("until"));

    runner.tick(0.016f); // Still true, so wait_while keeps waiting.
    EXPECT_EQ(t.log.size(), 1u);

    t.flag = false;
    runner.tick(0.016f);
    ASSERT_EQ(t.log.size(), 2u);
    EXPECT_EQ(t.log[1], std::string("while"));
}

COOPA_TEST(nested_routine_starts_immediately_and_returns_in_the_same_tick) {
    cr::RoutineRunner runner;
    Trace t;
    auto handle = runner.start(nested_parent(&t));

    // The child starts immediately, exactly as `yield return StartCoroutine(x)` does.
    ASSERT_EQ(t.log.size(), 2u);
    EXPECT_EQ(t.log[0], std::string("parent-begin"));
    EXPECT_EQ(t.log[1], std::string("child-begin"));

    // The child finishes and hands control straight back to the parent, in the
    // same tick -- libcoopa does not spend a frame per nesting level.
    runner.tick(0.016f);
    ASSERT_EQ(t.log.size(), 4u);
    EXPECT_EQ(t.log[2], std::string("child-end"));
    EXPECT_EQ(t.log[3], std::string("parent-end"));
    EXPECT_FALSE(handle.is_running());
}

COOPA_TEST(stop_destroys_the_frame_and_unwinds_locals) {
    cr::RoutineRunner runner;
    Trace t;
    int destructions = 0;
    auto handle = runner.start(holds_a_local(&t, &destructions));

    runner.tick(0.016f);
    EXPECT_EQ(t.steps, 2);
    EXPECT_EQ(destructions, 0);

    EXPECT_TRUE(handle.stop());
    EXPECT_EQ(destructions, 1); // The frame was destroyed, so the local unwound.
    EXPECT_FALSE(handle.is_running());
    EXPECT_EQ(runner.active_count(), 0u);

    runner.tick(0.016f);
    EXPECT_EQ(t.steps, 2);       // Stopped for good.
    EXPECT_FALSE(handle.stop()); // Stopping twice is a harmless no-op.
}

COOPA_TEST(routine_can_stop_itself_from_inside_its_body) {
    cr::RoutineRunner runner;
    SelfStop s;
    s.runner = &runner;
    s.handle = runner.start(stops_itself(&s));
    EXPECT_EQ(s.steps, 1);

    runner.tick(0.016f); // Resumes, stops itself mid-body, then suspends.
    EXPECT_EQ(s.steps, 2);
    EXPECT_FALSE(s.after_stop);
    EXPECT_FALSE(s.handle.is_running());

    runner.tick(0.016f);
    EXPECT_FALSE(s.after_stop);
    EXPECT_EQ(runner.active_count(), 0u);
}

COOPA_TEST(routine_started_mid_tick_joins_the_pump_next_tick) {
    cr::RoutineRunner runner;
    Trace t;
    runner.start(starts_a_sibling(&t, &runner));
    EXPECT_EQ(t.log.size(), 1u);
    EXPECT_EQ(t.steps, 0);

    runner.tick(0.016f);
    // The sibling was started from inside a tick: its body ran immediately up
    // to the first suspension, but the pump already walking the active list
    // does not resume it again this tick.
    EXPECT_EQ(t.steps, 1);
    EXPECT_EQ(t.log.size(), 1u);
    EXPECT_EQ(runner.active_count(), 2u);

    runner.tick(0.016f); // The sibling has joined the pump.
    EXPECT_EQ(t.steps, 2);
    ASSERT_EQ(t.log.size(), 2u);
    EXPECT_EQ(t.log[1], std::string("outer-end"));
    EXPECT_EQ(runner.active_count(), 1u);

    runner.tick(0.016f);
    EXPECT_TRUE(t.reached_end);
    EXPECT_EQ(runner.active_count(), 0u);
}

COOPA_TEST(scope_destruction_stops_its_routines) {
    cr::RoutineRunner runner;
    Trace t;
    cr::RoutineHandle handle;
    {
        cr::RoutineScope scope;
        handle = scope.start(runner, count_frames(&t, 1000));
        runner.tick(0.016f);
        EXPECT_EQ(t.steps, 2);
        EXPECT_EQ(scope.active_count(), 1u);
    }
    EXPECT_FALSE(handle.is_running());
    runner.tick(0.016f);
    EXPECT_EQ(t.steps, 2);
    EXPECT_EQ(runner.active_count(), 0u);
}

COOPA_TEST(handle_outliving_its_runner_degrades_to_not_running) {
    Trace t;
    cr::RoutineHandle handle;
    {
        cr::RoutineRunner runner;
        handle = runner.start(count_frames(&t, 1000));
        EXPECT_TRUE(handle.is_running());
    }
    // The runner is gone: the handle degrades to "not running" rather than dangling.
    EXPECT_FALSE(handle.is_running());
    EXPECT_FALSE(handle.stop());
    EXPECT_EQ(t.steps, 1);
}

COOPA_TEST(wait_for_job_handle_resumes_only_after_the_job_completes) {
    coopa::job::JobEngine engine(2);
    cr::RoutineRunner runner(&engine);
    Trace t;

    std::atomic<bool> release{false};
    coopa::job::JobHandle handle = engine.create_handle();
    engine.submit([&release]() {
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    }, 0, handle);

    runner.start(awaits_job(&t, handle));
    EXPECT_EQ(t.log.size(), 1u);

    for (int i = 0; i < 20; ++i) runner.tick(0.016f);
    EXPECT_EQ(t.log.size(), 1u); // Still blocked on the job.

    release.store(true, std::memory_order_release);
    engine.wait_for(handle);
    runner.tick(0.016f);
    ASSERT_EQ(t.log.size(), 2u);
    EXPECT_EQ(t.log[1], std::string("after"));

    handle.close(); // wait_for() never closes a handle it did not allocate.
}

COOPA_TEST(on_worker_runs_off_thread_and_resumes_on_the_tick_thread) {
    coopa::job::JobEngine engine(2);
    cr::RoutineRunner runner(&engine);

    ThreadProbe p;
    p.caller = std::this_thread::get_id();
    auto handle = runner.start(hops_to_worker(&p));

    for (int i = 0; i < 2000 && handle.is_running(); ++i) {
        runner.tick(0.001f);
        if (handle.is_running()) std::this_thread::yield();
    }

    ASSERT_FALSE(handle.is_running());
    EXPECT_TRUE(p.body_ran);
    EXPECT_TRUE(p.worker != p.caller);  // The body ran on a worker thread.
    EXPECT_TRUE(p.resumed == p.caller); // The routine came back to the tick thread.
}

COOPA_TEST(on_worker_without_an_engine_runs_inline) {
    cr::RoutineRunner runner; // No engine installed.
    ThreadProbe p;
    p.caller = std::this_thread::get_id();
    auto handle = runner.start(hops_to_worker(&p));

    EXPECT_TRUE(p.body_ran); // Ran inline the moment it was yielded.
    EXPECT_TRUE(p.worker == p.caller);
    EXPECT_TRUE(handle.is_running());

    runner.tick(0.016f);
    EXPECT_FALSE(handle.is_running());
    EXPECT_TRUE(p.resumed == p.caller);
}

COOPA_TEST(exception_in_a_body_stops_that_routine_and_is_kept_not_rethrown) {
    cr::RoutineRunner runner;
    Trace t;
    auto handle = runner.start(throws_after_a_yield(&t));
    EXPECT_TRUE(runner.last_exception() == nullptr);

    runner.tick(0.016f); // Throws here; logged, not rethrown out of tick().
    EXPECT_FALSE(handle.is_running());
    EXPECT_EQ(runner.active_count(), 0u);
    ASSERT_TRUE(runner.last_exception() != nullptr);

    bool caught = false;
    try {
        std::rethrow_exception(runner.last_exception());
    } catch (const std::runtime_error& e) {
        caught = (std::string(e.what()) == "routine blew up");
    }
    EXPECT_TRUE(caught);

    runner.clear_last_exception();
    EXPECT_TRUE(runner.last_exception() == nullptr);
}
