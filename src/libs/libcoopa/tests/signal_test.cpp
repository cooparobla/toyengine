/**
 * @file signal_test.cpp
 * @brief coopa::event::Signal: emit/disconnect ordering, scoped connections, and the
 *        reentrancy/lifetime cases (self-disconnect, connect during emit, connection outliving
 *        the signal, signal destroyed from its own slot) that were use-after-free class bugs.
 */
#include <coopa/testing/test.h>

#include <coopa/event/signal.h>

#include <memory>
#include <utility>

COOPA_TEST_SUITE("signal");

using coopa::event::Connection;
using coopa::event::ScopedConnection;
using coopa::event::Signal;
using coopa::event::k_invalid_slot;

COOPA_TEST(emit_reaches_every_slot_and_empty_callables_are_ignored) {
    Signal<int> s;
    int a = 0, b = 0;
    s.connect([&](int v) { a += v; });
    s.connect([&](int v) { b += v; });
    EXPECT_EQ(s.slot_count(), 2u);

    s.emit(5);
    EXPECT_TRUE(a == 5 && b == 5);
    s.emit(5);
    EXPECT_TRUE(a == 10 && b == 10);

    Signal<> empty_signal;
    EXPECT_EQ(empty_signal.slot_count(), 0u);
    empty_signal.emit(); // no-op, must not throw/crash

    auto c = empty_signal.connect({}); // empty std::function is ignored
    EXPECT_TRUE(c.id() == k_invalid_slot);
    EXPECT_EQ(empty_signal.slot_count(), 0u);
}

COOPA_TEST(disconnect_removes_one_slot_and_preserves_order) {
    Signal<int> s;
    int a = 0, b = 0, c = 0;
    s.connect([&](int v) { a += v; });
    auto cb = s.connect([&](int v) { b += v; });
    s.connect([&](int v) { c += v; });

    ASSERT_TRUE(cb.disconnect());
    EXPECT_EQ(s.slot_count(), 2u);

    s.emit(1);
    EXPECT_TRUE(a == 1 && b == 0 && c == 1); // order preserved after removing the middle slot

    EXPECT_FALSE(cb.connected());
    EXPECT_FALSE(cb.disconnect());     // already gone
    EXPECT_FALSE(s.disconnect(99999)); // unknown id
}

COOPA_TEST(scoped_connection_disconnects_on_scope_exit_and_move_assign) {
    Signal<> s;
    int count = 0;
    {
        ScopedConnection sc = s.connect_scoped([&] { ++count; });
        EXPECT_EQ(s.slot_count(), 1u);
        s.emit();
    }
    EXPECT_EQ(s.slot_count(), 0u); // scope exit disconnected it
    s.emit();
    EXPECT_EQ(count, 1);

    ScopedConnection outer;
    {
        ScopedConnection inner = s.connect_scoped([&] { ++count; });
        outer = std::move(inner); // move-construct/assign out of the inner scope
    }
    EXPECT_EQ(s.slot_count(), 1u);
    s.emit();
    EXPECT_EQ(count, 2);

    ScopedConnection other = s.connect_scoped([&] { ++count; });
    EXPECT_EQ(s.slot_count(), 2u);
    outer = std::move(other); // move-assign over a live connection disconnects the old one
    EXPECT_EQ(s.slot_count(), 1u);

    auto released = outer.release(); // gives up ownership without disconnecting
    EXPECT_TRUE(released.connected());
}

COOPA_TEST(slot_may_disconnect_itself_during_emit) {
    Signal<> s;
    int calls = 0;
    int after_disconnect_marker = 0;
    Connection self;
    self = s.connect([&] {
        ++calls;
        self.disconnect();
        // If Signal destroyed the running std::function on disconnect() (rather than
        // tombstoning it), this write would be use-after-free.
        after_disconnect_marker = 42;
    });

    s.emit();
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(after_disconnect_marker, 42);
    s.emit();
    EXPECT_EQ(calls, 1); // it really disconnected
}

COOPA_TEST(connect_and_disconnect_during_emit_apply_from_the_next_emit) {
    Signal<> s;
    int a_calls = 0, b_calls = 0, c_calls = 0;
    Connection c_conn;
    s.connect([&] {
        ++a_calls;
        s.connect([&] { ++b_calls; }); // connecting mid-emit must not run this emit
        if (c_conn.id() != k_invalid_slot) c_conn.disconnect();
    });
    c_conn = s.connect([&] { ++c_calls; });

    s.emit();
    EXPECT_EQ(a_calls, 1);
    EXPECT_EQ(b_calls, 0);         // connected during this emit: runs next time, not now
    EXPECT_EQ(c_calls, 0);         // disconnected earlier in this same emit: skipped
    EXPECT_EQ(s.slot_count(), 2u); // a + b; c was removed

    s.emit();
    EXPECT_EQ(b_calls, 1);
}

COOPA_TEST(disconnect_all_kills_old_tokens_and_ids_are_never_reused) {
    Signal<> s;
    auto c1 = s.connect([] {});
    auto c2 = s.connect([] {});
    auto c3 = s.connect([] {});

    s.disconnect_all();
    EXPECT_TRUE(s.empty());
    s.emit(); // no-op
    EXPECT_TRUE(!c1.connected() && !c2.connected() && !c3.connected());

    int fired = 0;
    s.connect([&] { ++fired; });
    s.emit();
    EXPECT_EQ(fired, 1);
    EXPECT_FALSE(c1.connected()); // ids are never reused, so the old token stays dead
}

COOPA_TEST(connections_outliving_their_signal_are_inert) {
    auto sig  = std::make_unique<Signal<>>();
    auto conn = sig->connect([] {});
    ScopedConnection sc = sig->connect_scoped([] {});

    sig.reset();

    EXPECT_FALSE(conn.connected());
    EXPECT_FALSE(conn.disconnect());
    // sc's destructor runs at scope exit and must not crash on the dead signal.
}

COOPA_TEST(signal_destroyed_from_its_own_slot_unwinds_cleanly) {
    auto sig = std::make_unique<Signal<>>();
    bool ran = false;
    sig->connect([&] {
        ran = true;
        sig.reset(); // destroys the Signal while its own emit() is on the stack
    });

    sig->emit(); // must unwind cleanly rather than touching *sig afterward
    EXPECT_TRUE(ran);
}

COOPA_TEST(reference_arguments_are_forwarded_not_copied) {
    struct Payload {
        int copies = 0;
        Payload() = default;
        Payload(const Payload& other) : copies(other.copies + 1) {}
    };

    Signal<const Payload&> s;
    int seen_copies = 0;
    s.connect([&](const Payload& p) { seen_copies += p.copies; });
    s.connect([&](const Payload& p) { seen_copies += p.copies; });

    Payload p;
    s.emit(p);
    EXPECT_EQ(seen_copies, 0); // emit() forwards the reference, never copies the argument
}
