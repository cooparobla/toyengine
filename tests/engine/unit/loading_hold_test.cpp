/**
 * @file loading_hold_test.cpp
 * @brief LoadingState / LoadingState::Hold (toyengine/core/loading.h): acquire/release, move
 *        semantics, multiple holds, and loading_reveal_ready()'s interaction with held().
 */

#include <coopa/testing/test.h>

#include <utility>

#include <toyengine/core/loading.h>
#include <toyengine/core/scene_loading.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("loading_hold");

using toy::core::LoadingState;

COOPA_TEST(acquire_holds_and_release_drops) {
    LoadingState state;
    expect(!state.held() && state.hold_count() == 0, "a fresh LoadingState has no holds");

    LoadingState::Hold hold = state.acquire("importing save");
    expect(hold.active(), "acquire() returns an active hold");
    expect(state.held() && state.hold_count() == 1, "the state sees the open hold");
    expect(state.reasons().size() == 1 && state.reasons()[0] == "importing save",
           "reasons() reports the hold's reason");

    hold.release();
    expect(!hold.active(), "release() deactivates the hold");
    expect(!state.held() && state.hold_count() == 0, "release() drops the state's hold too");

    hold.release();   // idempotent
    expect(!state.held(), "a second release() is a harmless no-op");
}

COOPA_TEST(going_out_of_scope_releases_too) {
    LoadingState state;
    {
        LoadingState::Hold hold = state.acquire("scoped work");
        expect(state.held(), "the hold is open inside its scope");
    }
    expect(!state.held(), "~Hold() released it at the end of scope");
}

COOPA_TEST(multiple_holds_all_must_release) {
    LoadingState state;
    LoadingState::Hold a = state.acquire("a");
    LoadingState::Hold b = state.acquire("b");
    expect(state.hold_count() == 2, "two opens means two holds");
    a.release();
    expect(state.held() && state.hold_count() == 1, "one release leaves the other open");
    b.release();
    expect(!state.held(), "the last release clears it");
}

COOPA_TEST(move_transfers_ownership_without_double_releasing) {
    LoadingState state;
    LoadingState::Hold a = state.acquire("movable");
    LoadingState::Hold b = std::move(a);
    expect(!a.active() && b.active(), "move leaves the source inactive and the target active");
    expect(state.hold_count() == 1, "moving does not open a second hold");
    b.release();
    expect(!state.held(), "releasing the moved-to hold clears it");

    // Move-assignment over an already-active hold releases the one it is replacing.
    LoadingState::Hold c = state.acquire("c");
    LoadingState::Hold d = state.acquire("d");
    c = std::move(d);
    expect(state.hold_count() == 1, "move-assignment released the overwritten hold first");
}

COOPA_TEST(copies_of_a_loading_state_share_the_same_holds) {
    LoadingState state;
    LoadingState alias = state;   // Engine::loading() returns a reference normally, but the
                                   // type itself is a cheap-to-copy handle onto shared state.
    LoadingState::Hold hold = state.acquire("shared");
    expect(alias.held(), "a copy of LoadingState observes holds taken through the original");
}

COOPA_TEST(loading_reveal_ready_blocks_on_a_held_hold_even_past_a_timeout) {
    using toy::core::loading_reveal_ready;
    expect(loading_reveal_ready(/*warm=*/true, /*timed_out=*/false, /*held=*/false, /*min_display_met=*/true),
           "warm and not held: ready");
    expect(loading_reveal_ready(/*warm=*/false, /*timed_out=*/true, /*held=*/false, /*min_display_met=*/true),
           "timed out and not held: ready anyway");
    expect(!loading_reveal_ready(/*warm=*/true, /*timed_out=*/false, /*held=*/true, /*min_display_met=*/true),
           "warm but held: not ready -- a manual hold is not defeated by warmth");
    expect(!loading_reveal_ready(/*warm=*/false, /*timed_out=*/true, /*held=*/true, /*min_display_met=*/true),
           "timed out but held: not ready -- a manual hold is not defeated by the timeout escape hatch");
    expect(!loading_reveal_ready(/*warm=*/false, /*timed_out=*/false, /*held=*/false, /*min_display_met=*/true),
           "neither warm nor timed out: not ready");
}

COOPA_TEST(loading_reveal_ready_blocks_on_min_display_time_even_past_a_timeout) {
    using toy::core::loading_reveal_ready;
    expect(!loading_reveal_ready(/*warm=*/true, /*timed_out=*/false, /*held=*/false, /*min_display_met=*/false),
           "warm but min display time not yet met: not ready");
    expect(!loading_reveal_ready(/*warm=*/false, /*timed_out=*/true, /*held=*/false, /*min_display_met=*/false),
           "timed out but min display time not yet met: not ready -- the timeout is for a system "
           "that never reports warm, not a reason to flash the cover");
    expect(loading_reveal_ready(/*warm=*/true, /*timed_out=*/false, /*held=*/false, /*min_display_met=*/true),
           "warm and min display time met: ready");
}
