/**
 * @file dependency_graph_test.cpp
 * @brief Job dependencies (coopa/job/dependency_graph.h through JobEngine::submit): chains run
 *        in order, fan-in waits for every dependency (beyond the old 4-dependency inline cap),
 *        a reused handle's second wave is waited for, and the WaiterNode lifetime race.
 *
 * The short sleeps inside dependency bodies are what make a broken dependency observable: the
 * dependent would otherwise run while the dependency is still sleeping. They are kept to a few
 * milliseconds.
 */
#include <coopa/testing/test.h>

#include <coopa/job/dependency_graph.h>
#include <coopa/job/engine.h>
#include <coopa/job/handle.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

COOPA_TEST_SUITE("dependency_graph");

COOPA_TEST(chained_dependencies_run_strictly_in_order) {
    coopa::job::JobEngine engine(4);
    engine.begin_frame();

    // A must run before B, B before C (transitively after A). Each step CASes the expected
    // predecessor value, so any reordering leaves order_correct false.
    std::atomic<int>  sequence{0};
    std::atomic<bool> order_correct{true};
    auto advance = [&](int from, int to) {
        int expected = from;
        if (!sequence.compare_exchange_strong(expected, to)) order_correct.store(false);
    };

    coopa::job::JobHandle handle_a = engine.create_handle();
    engine.submit([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        advance(0, 1);
    }, 1, handle_a);

    coopa::job::JobHandle handle_b = engine.create_handle();
    coopa::job::JobHandle deps_b[] = {handle_a};
    engine.submit([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        advance(1, 2);
    }, 1, handle_b, deps_b, 1);

    coopa::job::JobHandle handle_c = engine.create_handle();
    coopa::job::JobHandle deps_c[] = {handle_b};
    engine.submit([&]() { advance(2, 3); }, 1, handle_c, deps_c, 1);

    engine.wait_for(handle_c);
    EXPECT_EQ(sequence.load(), 3);
    EXPECT_TRUE(order_correct.load());

    engine.end_frame();
}

COOPA_TEST(fan_in_waits_for_all_sixteen_dependencies) {
    // The previous design silently truncated dependency lists to
    // k_max_inline_dependencies (4); this exercises 16 to prove that cliff
    // is gone.
    coopa::job::JobEngine engine(4);
    constexpr int kDeps = 16;
    std::atomic<int> completed{0};
    std::vector<coopa::job::JobHandle> deps;
    for (int i = 0; i < kDeps; ++i) {
        auto h = engine.create_handle();
        engine.submit([&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            completed.fetch_add(1, std::memory_order_release);
        }, 0, h);
        deps.push_back(h);
    }

    coopa::job::JobHandle final_handle = engine.create_handle();
    bool all_done_before_final = false;
    engine.submit([&]() {
        all_done_before_final = (completed.load(std::memory_order_acquire) == kDeps);
    }, 0, final_handle, deps.data(), static_cast<uint32_t>(deps.size()));

    engine.wait_for(final_handle);
    EXPECT_EQ(completed.load(), kDeps);
    EXPECT_TRUE(all_done_before_final);

    for (auto& h : deps) h.close();
    final_handle.close();
}

// A handle may be reused after its counter has already reached zero (the
// fan-in pattern in handle.h's doc: submit, let it finish, submit more against
// the same handle). A dependent registered against the SECOND wave must wait
// for that wave, not be told the handle is already complete because the first
// wave finished.
COOPA_TEST(dependent_on_reused_handle_waits_for_the_second_wave) {
    for (int iteration = 0; iteration < 10; ++iteration) {
        coopa::job::JobEngine engine(2);
        coopa::job::JobHandle shared = engine.create_handle();

        // Wave 1: drive the counter to zero.
        engine.submit([]() {}, 1, shared);
        engine.wait_for(shared);

        // Wave 2: same handle, slow enough that an early-firing dependent
        // would observe the flag still unset.
        std::atomic<bool> wave2_done{false};
        engine.submit([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            wave2_done.store(true);
        }, 1, shared);

        std::atomic<bool> saw_wave2{false};
        coopa::job::JobHandle dependent = engine.create_handle();
        engine.submit([&]() { saw_wave2.store(wave2_done.load()); }, 1, dependent, &shared, 1);

        engine.wait_for(dependent);
        ASSERT_TRUE(saw_wave2.load());

        shared.close();
        dependent.close();
        engine.shutdown();
    }
}

// Hammers the interleaving where a WaiterNode is held by two threads at once:
// submit_with_dependencies()'s phase-2 re-check and a completer's
// harvest_and_fire(). Either may win the node's `fired` CAS and drive `unmet`
// to zero; the loser still dereferences the node afterwards, so freeing it at
// that moment is a use-after-free.
//
// Every dependency is completed by its own thread, all released from a spin
// barrier at the instant the submitting thread enters phase 1, so the final
// `unmet` decrement lands while other threads are still walking their harvest.
// Best run under ASan/TSan; what it asserts unconditionally is that the
// dependent job runs exactly once -- never lost, never doubled.
//
// The completers yield while waiting for `go` (a hard spin with the engine's 4 workers
// oversubscribed an 8-core machine and made this take 1-4 s), and 1000 iterations (was 3000)
// keep it ~0.2 s; each iteration exercises the same interleaving.
COOPA_TEST(waiter_node_outlives_racing_completers) {
    constexpr int k_iterations = 1000;
    constexpr int k_deps       = 8;

    coopa::job::JobEngine engine(4);

    for (int iteration = 0; iteration < k_iterations; ++iteration) {
        coopa::job::JobHandle deps[k_deps];
        for (int i = 0; i < k_deps; ++i) {
            deps[i] = engine.create_handle();
            deps[i].add_jobs_(1); // Completed by hand below, not by a queued task.
        }

        std::atomic<int>  dependent_runs{0};
        std::atomic<bool> go{false};
        coopa::job::JobHandle dependent = engine.create_handle();

        std::vector<std::thread> completers;
        completers.reserve(k_deps);
        for (int i = 0; i < k_deps; ++i) {
            completers.emplace_back([&, i]() {
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                engine.complete_external_job(deps[i]);
            });
        }

        go.store(true, std::memory_order_release);
        engine.submit([&]() { dependent_runs.fetch_add(1); }, 1, dependent, deps, k_deps);

        for (std::thread& t : completers) t.join();

        engine.wait_for(dependent);
        ASSERT_EQ(dependent_runs.load(), 1); // Ran exactly once: not lost, not doubled.

        dependent.close();
        for (int i = 0; i < k_deps; ++i) deps[i].close();
    }

    engine.shutdown();
}
