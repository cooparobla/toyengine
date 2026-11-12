/**
 * @file work_stealing_deque_test.cpp
 * @brief coopa::job::WorkStealingDeque: owner LIFO / thief FIFO contract, and the two
 *        concurrency regressions (torn moves on the last element; double/lost consumption under
 *        full-ring pressure).
 *
 * The contention tests assert "consumed exactly once"; they are most useful under TSan/ASan but
 * the counts catch the historical bugs without them.
 */
#include <coopa/testing/test.h>

#include <coopa/job/collections/work_stealing_deque.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

COOPA_TEST_SUITE("work_stealing_deque");

COOPA_TEST(owner_pops_lifo_and_thieves_steal_fifo) {
    coopa::job::WorkStealingDeque<int> deque(16);
    EXPECT_TRUE(deque.empty_approx());
    EXPECT_EQ(deque.size_approx(), 0u);

    ASSERT_TRUE(deque.push(10));
    ASSERT_TRUE(deque.push(20));
    ASSERT_TRUE(deque.push(30));
    EXPECT_EQ(deque.size_approx(), 3u);

    int val = 0;
    ASSERT_TRUE(deque.pop(val));
    EXPECT_EQ(val, 30); // LIFO from bottom.
    ASSERT_TRUE(deque.steal(val));
    EXPECT_EQ(val, 10); // FIFO from top.
    EXPECT_EQ(deque.size_approx(), 1u);
    ASSERT_TRUE(deque.pop(val));
    EXPECT_EQ(val, 20);

    EXPECT_TRUE(deque.empty_approx());
    EXPECT_FALSE(deque.pop(val));
    EXPECT_FALSE(deque.steal(val));

    deque.push(99);
    deque.clear();
    EXPECT_TRUE(deque.empty_approx());
}

COOPA_TEST(no_torn_moves_when_owner_and_thief_race_for_the_last_item) {
    // pop() and steal() must each win their CAS on top_ BEFORE moving the item
    // out of buffer_[idx]. On the contended "exactly one item left" path, the
    // loser (owner pop() vs. a thief, or two racing thieves) would otherwise
    // perform an unsynchronized move-read of the same slot the winner is also
    // moving from. A plain int can't reveal this (a racy read of a scalar
    // just yields a value, not corruption); a type whose move leaves the
    // source in a detectably-different state can.
    using coopa::job::WorkStealingDeque;

    struct NullingMovable {
        int  value = -1;
        bool alive = false;
        NullingMovable() = default;
        explicit NullingMovable(int v) : value(v), alive(true) {}
        NullingMovable(NullingMovable&& other) noexcept : value(other.value), alive(other.alive) {
            other.alive = false;
            other.value = -1;
        }
        NullingMovable& operator=(NullingMovable&& other) noexcept {
            value = other.value;
            alive = other.alive;
            other.alive = false;
            other.value = -1;
            return *this;
        }
        NullingMovable(const NullingMovable&)            = delete;
        NullingMovable& operator=(const NullingMovable&) = delete;
    };

    const int k_trials = 3000;
    for (int trial = 0; trial < k_trials; ++trial) {
        WorkStealingDeque<NullingMovable> deque(64);
        deque.push(NullingMovable(42));

        std::atomic<int> success_count{0};
        std::atomic<int> phantom_count{0};

        auto thief = [&]() {
            NullingMovable val;
            if (deque.steal(val)) {
                success_count.fetch_add(1);
                if (!val.alive) phantom_count.fetch_add(1);
            }
        };
        auto owner_pop = [&]() {
            NullingMovable val;
            if (deque.pop(val)) {
                success_count.fetch_add(1);
                if (!val.alive) phantom_count.fetch_add(1);
            }
        };

        std::thread t1(thief);
        std::thread t2(owner_pop);
        t1.join();
        t2.join();

        ASSERT_EQ(success_count.load(), 1);
        ASSERT_EQ(phantom_count.load(), 0);
    }
}

/**
 * Every item is consumed exactly once while the owner pushes and pops and four thieves steal,
 * including under full-ring pressure (the owner keeps it near capacity, so it wraps constantly).
 * Regression for two bugs that ran jobs twice and lost others: pop()'s last-element path
 * restoring bottom_ from the CAS-clobbered `t` (a phantom element), and a preempted thief whose
 * slot the owner overwrote after a lap (fixed by the per-slot busy flag).
 */
COOPA_TEST(every_item_consumed_exactly_once_under_steal_pressure) {
    for (int pressure = 0; pressure < 2; ++pressure) {
        const int n = 400000;
        std::vector<std::atomic<uint8_t>> seen(n);
        coopa::job::WorkStealingDeque<int> dq(64);
        std::atomic<bool> stop{false};
        std::vector<std::thread> thieves;
        for (int k = 0; k < 4; ++k) {
            thieves.emplace_back([&]() {
                int v;
                while (!stop.load(std::memory_order_relaxed)) {
                    if (dq.steal(v)) seen[static_cast<size_t>(v)].fetch_add(1);
                }
            });
        }
        int next = 0;
        while (next < n) {
            if (pressure == 1 || dq.size_approx() < 16) {
                for (int i = 0; i < 1 + next % 7 && next < n; ++i) {
                    int x = next;
                    if (dq.push(std::move(x))) ++next;
                }
            }
            int v;
            for (int i = 0; i < next % 3; ++i) {
                if (dq.pop(v)) seen[static_cast<size_t>(v)].fetch_add(1);
            }
        }
        int v;
        while (dq.pop(v)) seen[static_cast<size_t>(v)].fetch_add(1);
        while (!dq.empty_approx()) std::this_thread::yield();
        // No settle sleep needed: a thief mid-steal() finishes its loop iteration (and records
        // what it took) before it re-reads `stop`, and join() waits for exactly that.
        stop = true;
        for (auto& th : thieves) th.join();
        int wrong = 0;
        for (int i = 0; i < n; ++i) wrong += seen[static_cast<size_t>(i)].load() != 1;
        EXPECT_EQ(wrong, 0);
    }
}
