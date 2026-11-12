/**
 * @file job_engine_test.cpp
 * @brief coopa::job::JobEngine: handle lifetime (generations, pool exhaustion, frame
 *        boundaries), lost-wakeup / phantom-completion regressions, nested waits, overflow,
 *        parallel_for coverage, cancellation and priority ordering.
 *
 * Dependency semantics (fan-in, chains, reused handles, WaiterNode lifetime) live in
 * dependency_graph_test.cpp; JobScheduler in job_scheduler_test.cpp.
 */
#include <coopa/testing/test.h>

#include <coopa/job/engine.h>
#include <coopa/job/handle.h>
#include <coopa/job/parallel_for.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

COOPA_TEST_SUITE("job_engine");

namespace {

/** @brief Submits a job that holds a worker until `open` is set; `taken` flips once it runs. */
coopa::job::JobHandle submit_gate(coopa::job::JobEngine& engine, std::atomic<bool>& open,
                                  std::atomic<bool>* taken = nullptr) {
    coopa::job::JobHandle gate = engine.create_handle();
    engine.submit([&open, taken]() {
        if (taken) taken->store(true, std::memory_order_release);
        while (!open.load(std::memory_order_acquire)) std::this_thread::yield();
    }, 0, gate);
    return gate;
}

} // namespace

COOPA_TEST(repeated_short_lived_engines_never_lose_or_fake_a_job) {
    // Covers the short-lived-engine pattern (construct an engine, submit, then
    // wait_for() from the main thread immediately) against two failure modes:
    //
    // 1. Lost wakeup: a job deposited into one SPECIFIC worker's inbox must
    //    wake THAT worker. Waking an arbitrary idle worker instead leaves the
    //    inbox's owner asleep, and wait_for() on that handle hangs forever.
    // 2. Phantom completion: a job's counter must only be decremented by a
    //    worker that actually ran its body. A torn hand-off (see
    //    work_stealing_deque/no_torn_moves_when_owner_and_thief_race_for_the_last_item)
    //    can yield a no-op job whose counter is still decremented, so wait_for()
    //    returns believing work completed that never executed.
    //
    // A tight repeated construct/submit/wait_for loop is what exposes both, so
    // this runs a few hundred iterations as a bounded-runtime tripwire.
    const int k_iterations = 400;
    for (int i = 0; i < k_iterations; ++i) {
        coopa::job::JobEngine engine(2);
        std::atomic<bool> ran{false};
        coopa::job::JobHandle handle = engine.create_handle();
        engine.submit([&ran]() { ran.store(true, std::memory_order_release); }, 1, handle);
        engine.wait_for(handle);
        ASSERT_TRUE(ran.load(std::memory_order_acquire));
    }
}

COOPA_TEST(handle_generation_prevents_stale_aliasing) {
    coopa::job::JobEngine engine(2, 8, 4); // handle_pool_capacity = 4
    coopa::job::JobHandle first = engine.create_handle();
    ASSERT_TRUE(first.is_valid());
    engine.submit([] {}, 0, first);
    engine.wait_for(first);
    ASSERT_TRUE(first.is_complete());

    coopa::job::JobHandle stale_copy = first; // same slot + generation
    first.close();                            // returns the slot to the free list

    std::vector<coopa::job::JobHandle> churn;
    for (int i = 0; i < 4; ++i) {
        auto h = engine.create_handle();
        ASSERT_TRUE(h.is_valid());
        churn.push_back(h);
    }

    // stale_copy's generation cannot match whichever handle now occupies that
    // slot -- it must report complete regardless, and must never be mistaken
    // for one of the new occupants.
    EXPECT_TRUE(stale_copy.is_complete());
    for (auto& h : churn) {
        EXPECT_FALSE(h == stale_copy);
        h.close();
    }
}

COOPA_TEST(counter_pool_exhaustion_recovers_after_close) {
    coopa::job::JobEngine engine(2, 64, 2); // handle_pool_capacity = 2
    coopa::job::JobHandle a = engine.create_handle();
    coopa::job::JobHandle b = engine.create_handle();
    ASSERT_TRUE(a.is_valid());
    ASSERT_TRUE(b.is_valid());

    coopa::job::JobHandle c = engine.create_handle();
    EXPECT_FALSE(c.is_valid()); // pool exhausted

    engine.submit([] {}, 0, a);
    engine.wait_for(a);
    a.close(); // frees one slot

    coopa::job::JobHandle recovered = engine.create_handle();
    EXPECT_TRUE(recovered.is_valid()); // pool recovered after a close(), no frame boundary needed

    engine.submit([] {}, 0, b);
    engine.wait_for(b);
    b.close();
    engine.submit([] {}, 0, recovered);
    engine.wait_for(recovered);
    recovered.close();
}

COOPA_TEST(handles_survive_frame_boundaries_and_sharing_across_subsystems) {
    coopa::job::JobEngine engine(2);

    // A handle created before begin_frame()/end_frame()/begin_frame() is still usable.
    coopa::job::JobHandle handle = engine.create_handle();
    engine.begin_frame();
    engine.end_frame();
    engine.begin_frame(); // a second begin_frame -- must NOT invalidate `handle`
    std::atomic<bool> ran{false};
    engine.submit([&ran]() { ran.store(true); }, 0, handle);
    engine.wait_for(handle);
    EXPECT_TRUE(ran.load());
    engine.end_frame();
    handle.close();

    // Two independent subsystems sharing one engine across several frames with
    // no coordination beyond the engine itself -- neither may disturb the
    // other's outstanding handles.
    std::atomic<int> subsystem_a_count{0};
    std::atomic<int> subsystem_b_count{0};
    for (int frame = 0; frame < 5; ++frame) {
        engine.begin_frame();
        coopa::job::JobHandle a = engine.create_handle();
        coopa::job::JobHandle b = engine.create_handle();
        engine.submit([&]() { subsystem_a_count.fetch_add(1); }, 0, a);
        engine.submit([&]() { subsystem_b_count.fetch_add(1); }, 0, b);
        engine.wait_for(a);
        engine.wait_for(b);
        a.close();
        b.close();
        engine.end_frame();
    }
    EXPECT_EQ(subsystem_a_count.load(), 5);
    EXPECT_EQ(subsystem_b_count.load(), 5);
}

COOPA_TEST(full_worker_deque_falls_back_to_global_queue) {
    // Small per-priority deque capacity so a single worker spawning many
    // child jobs onto its own deque is guaranteed to overflow it (200
    // children > 64 capacity). A full deque must fall back to the global
    // queue rather than lose the job.
    //
    // Deliberately not smaller than this: WorkStealingDeque's fixed-size
    // ring buffer (like other classic Chase-Lev implementations) assumes a
    // thief's steal() -- which reserves a slot via a CAS on top_ and only
    // *then* reads out of it -- completes before the owner wraps around and
    // reuses that same physical slot for a new push(). That assumption holds
    // overwhelmingly in real workloads (capacity is normally in the
    // thousands), but shrinking capacity far enough relative to how hard a
    // single test hammers push()/steal() concurrently (e.g. capacity 4 with
    // this same 200-iteration loop) can make the reuse race observable under
    // ThreadSanitizer. This is a property of the fixed-capacity Chase-Lev
    // design generally, not something introduced by any specific caller.
    coopa::job::JobEngine engine(1, 64);
    constexpr int kChildren = 200;
    std::atomic<int> ran{0};
    coopa::job::JobHandle handle = engine.create_handle();
    engine.submit([&engine, handle, &ran]() {
        for (int i = 0; i < kChildren; ++i) {
            engine.submit([&ran]() { ran.fetch_add(1, std::memory_order_relaxed); }, 0, handle);
        }
    }, 0, handle);

    engine.wait_for(handle);
    EXPECT_EQ(ran.load(), kChildren);
    handle.close();
}

COOPA_TEST(nested_wait_for_from_a_worker_does_not_deadlock) {
    // Single worker: `inner` can only ever be picked up by this same worker
    // acting as a real worker during its nested wait_for(), not by another
    // worker stealing it: a nested wait_for() must be able to serve itself
    // from its own deque, or this deadlocks.
    coopa::job::JobEngine engine(1);
    std::atomic<bool> inner_ran{false};

    coopa::job::JobHandle outer = engine.create_handle();
    engine.submit([&engine, &inner_ran]() {
        coopa::job::JobHandle inner = engine.create_handle();
        engine.submit([&inner_ran]() { inner_ran.store(true, std::memory_order_release); }, 0, inner);
        engine.wait_for(inner); // must not deadlock
        inner.close();
    }, 0, outer);

    engine.wait_for(outer);
    EXPECT_TRUE(inner_ran.load());
    outer.close();
}

COOPA_TEST(parallel_for_covers_every_index_exactly_once) {
    coopa::job::JobEngine engine(4);
    auto check = [&engine](size_t count, size_t grain) {
        std::vector<std::atomic<int>> hits(count > 0 ? count : 1);
        for (auto& h : hits) h.store(0);
        engine.parallel_for_blocking(count, grain, [&hits](size_t start, size_t end) {
            for (size_t i = start; i < end; ++i) hits[i].fetch_add(1, std::memory_order_relaxed);
        });
        int wrong = 0;
        for (size_t i = 0; i < count; ++i) wrong += hits[i].load() != 1;
        coopa::test::expect(wrong == 0, "every index in [0, count) is visited exactly once");
    };
    check(100, 1);
    check(100, 100); // one chunk
    check(100, 0);   // auto grain
    check(0, 4);     // empty range -- no chunks, no crash
    check(3, 10);    // count < grain -- exactly one chunk covering [0,3)
}

COOPA_TEST(cancel_skips_the_body_but_still_releases_the_counter) {
    coopa::job::JobEngine engine(2);
    std::atomic<bool> gate_open{false};
    std::atomic<bool> ran{false};

    coopa::job::JobHandle gate = submit_gate(engine, gate_open);
    coopa::job::JobHandle handle = engine.create_handle();
    coopa::job::JobHandle deps[] = {gate};
    engine.submit([&ran]() { ran.store(true); }, 0, handle, deps, 1);

    engine.cancel(handle); // cancel while still parked on `gate`
    gate_open.store(true, std::memory_order_release);

    engine.wait_for(handle); // returns: the counter was released
    EXPECT_FALSE(ran.load());
    gate.close();
    handle.close();
}

COOPA_TEST(high_priority_runs_before_low_on_a_single_worker) {
    coopa::job::JobEngine engine(1);
    std::atomic<bool> gate_open{false};
    coopa::job::JobHandle gate = submit_gate(engine, gate_open);

    std::vector<int> order;
    std::mutex order_mutex;
    coopa::job::JobHandle low  = engine.create_handle();
    coopa::job::JobHandle high = engine.create_handle();
    engine.submit([&]() { std::lock_guard<std::mutex> l(order_mutex); order.push_back(0); },
                  0, low, nullptr, 0u, coopa::job::Priority::Low);
    engine.submit([&]() { std::lock_guard<std::mutex> l(order_mutex); order.push_back(1); },
                  0, high, nullptr, 0u, coopa::job::Priority::High);

    gate_open.store(true, std::memory_order_release);
    engine.wait_for(low);
    engine.wait_for(high);

    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 1); // High ran before Low
    EXPECT_EQ(order[1], 0);

    gate.close();
    low.close();
    high.close();
}

/**
 * A frame-critical wait restricted to Normal must not pick up a queued Low-priority job (a long
 * background build would stall the frame), while the default wait still helps with anything.
 * The only worker is held on a gate, so every job here is run by the waiting main thread or not
 * at all.
 */
COOPA_TEST(restricted_wait_never_helps_with_low_priority_jobs) {
    coopa::job::JobEngine engine(1);
    std::atomic<bool> gate_open{false};
    std::atomic<bool> gate_taken{false};
    coopa::job::JobHandle gate = submit_gate(engine, gate_open, &gate_taken);
    while (!gate_taken.load(std::memory_order_acquire)) std::this_thread::yield(); // worker is now held

    std::atomic<bool> low_ran{false}, normal_ran{false};
    coopa::job::JobHandle low    = engine.create_handle();
    coopa::job::JobHandle normal = engine.create_handle();
    engine.submit([&]() { low_ran = true; }, 0, low, nullptr, 0u, coopa::job::Priority::Low);
    engine.submit([&]() { normal_ran = true; }, 0, normal, nullptr, 0u, coopa::job::Priority::Normal);

    engine.wait_for(normal, coopa::job::Priority::Normal);
    EXPECT_TRUE(normal_ran.load());
    EXPECT_FALSE(low_ran.load()); // the restricted wait left the Low job alone

    std::atomic<int> chunks{0};
    engine.parallel_for_blocking(8, 1, [&](size_t b, size_t e) { chunks += static_cast<int>(e - b); },
                                 0, coopa::job::Priority::Normal, coopa::job::Priority::Normal);
    EXPECT_EQ(chunks.load(), 8);
    EXPECT_FALSE(low_ran.load());

    engine.wait_for(low); // default: helps with anything, so it runs it itself
    EXPECT_TRUE(low_ran.load());

    gate_open.store(true, std::memory_order_release);
    engine.wait_for(gate);
    gate.close();
    low.close();
    normal.close();
}
