/**
 * @file ring_buffer_test.cpp
 * @brief collections/ring_buffer.h: the lock-free SPSC ring that carries mixer commands and
 *        events across the audio thread boundary -- capacity limits, FIFO order across
 *        wraparound, and loss-free transfer between two real threads.
 */
#include <coopa/testing/test.h>

#include <sfxcoopa/collections/ring_buffer.h>

#include <cstdint>
#include <thread>
#include <vector>

COOPA_TEST_SUITE("ring_buffer");

COOPA_TEST(push_rejects_when_full_and_bulk_write_is_partial) {
    coopa::sfx::collections::RingBuffer<int> ints(4);
    for (int i = 0; i < 4; ++i) ASSERT_TRUE(ints.push(i));
    EXPECT_FALSE(ints.push(999));  // full: rejected, never overwrites unread data
    EXPECT_EQ(ints.available_write(), 0u);

    coopa::sfx::collections::RingBuffer<float> ring(8);
    float src[5] = {1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
    EXPECT_EQ(ring.write(src, 5), 5u);
    float more[5] = {6.0f, 7.0f, 8.0f, 9.0f, 10.0f};
    EXPECT_EQ(ring.write(more, 5), 3u);  // only 3 slots left (capacity 8, 5 used)
    float out[8] = {};
    ASSERT_EQ(ring.read(out, 8), 8u);
    for (int i = 0; i < 8; ++i) EXPECT_NEAR(out[i], static_cast<float>(i + 1), 1e-6f);
}

COOPA_TEST(wraparound_preserves_fifo_order) {
    coopa::sfx::collections::RingBuffer<int> ring(4);
    // Push/pop repeatedly to force the internal index past several wraps of
    // the physical buffer while capacity never shrinks below what's queued.
    int next_push = 0;
    int next_pop = 0;
    for (int round = 0; round < 100; ++round) {
        ASSERT_TRUE(ring.push(next_push++));
        int out = 0;
        ASSERT_TRUE(ring.pop(out));
        ASSERT_EQ(out, next_pop++);
    }
    EXPECT_EQ(ring.available_read(), 0u);
    int dummy = 0;
    EXPECT_FALSE(ring.pop(dummy));  // empty
}

COOPA_TEST(producer_and_consumer_threads_lose_and_duplicate_nothing) {
    // 1M ints across a real producer/consumer thread pair: zero loss, zero
    // duplication, values arrive in order.
    constexpr int kCount = 1'000'000;
    coopa::sfx::collections::RingBuffer<int> ring(1024);

    std::thread producer([&]() {
        int i = 0;
        while (i < kCount) {
            if (ring.push(i)) ++i;
        }
    });

    std::vector<int> received;
    received.reserve(kCount);
    std::thread consumer([&]() {
        int out;
        while (static_cast<int>(received.size()) < kCount) {
            if (ring.pop(out)) received.push_back(out);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(static_cast<int>(received.size()), kCount);
    int out_of_order = 0;
    for (int i = 0; i < kCount; ++i) {
        if (received[static_cast<size_t>(i)] != i) ++out_of_order;
    }
    EXPECT_EQ(out_of_order, 0);
}
