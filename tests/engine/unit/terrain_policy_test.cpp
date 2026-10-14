/**
 * @file terrain_policy_test.cpp
 * @brief The streaming policy's pure halves: the latched stream centre and the collision radius.
 *
 * Both decide what the streamer does with a whole ring of chunks, and both are a function of
 * nothing but coordinates -- so they are tested here, with no device, no camera and no world,
 * rather than inferred from a flight.
 */

#include <coopa/testing/test.h>

#include <toyengine/world/terrain_component.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("terrain_policy");

using toy::world::ChunkCoord;
using toy::world::StreamCentre;
using toy::world::k_recentre_frames;
using toy::world::latch_stream_centre;

COOPA_TEST(the_first_centre_is_adopted_at_once) {
    StreamCentre state;
    const ChunkCoord centre = latch_stream_centre(state, ChunkCoord{3, -2}, 4);
    expect(centre == ChunkCoord({3, -2}), "a terrain with no centre yet takes the one it is given");
    expect(state.valid, "and remembers that it has one");
}

COOPA_TEST(a_candidate_must_hold_before_the_region_moves) {
    StreamCentre state;
    latch_stream_centre(state, ChunkCoord{0, 0}, 4);

    for (int i = 1; i < k_recentre_frames; ++i) {
        const ChunkCoord centre = latch_stream_centre(state, ChunkCoord{1, 0}, 4);
        expect(centre == ChunkCoord({0, 0}),
               "frame " + std::to_string(i) + " of a new candidate does not move the region");
    }
    expect(latch_stream_centre(state, ChunkCoord{1, 0}, 4) == ChunkCoord({1, 0}),
           "the region moves once the candidate has held k_recentre_frames frames");
}

COOPA_TEST(a_candidate_that_flickers_never_moves_the_region) {
    StreamCentre state;
    latch_stream_centre(state, ChunkCoord{0, 0}, 4);

    // What a camera turning on the spot produced: a candidate swinging between neighbours.
    // Each one resets the other's count, so none of them ever accumulates a move.
    for (int i = 0; i < 40; ++i) {
        const ChunkCoord candidate = (i % 2 == 0) ? ChunkCoord{1, 0} : ChunkCoord{-1, 1};
        expect(latch_stream_centre(state, candidate, 4) == ChunkCoord({0, 0}),
               "an alternating candidate leaves the region where it is");
    }
}

COOPA_TEST(returning_to_the_centre_cancels_a_pending_move) {
    StreamCentre state;
    latch_stream_centre(state, ChunkCoord{0, 0}, 4);
    for (int i = 0; i < k_recentre_frames - 1; ++i) latch_stream_centre(state, ChunkCoord{1, 0}, 4);
    latch_stream_centre(state, ChunkCoord{0, 0}, 4);   // back where it started
    expect(latch_stream_centre(state, ChunkCoord{1, 0}, 4) == ChunkCoord({0, 0}),
           "the count starts again, so a near-miss does not leave the region one frame from moving");
}

COOPA_TEST(a_jump_past_half_the_view_recentres_immediately) {
    StreamCentre state;
    latch_stream_centre(state, ChunkCoord{0, 0}, 8);
    // Past half the radius the loaded region no longer comfortably covers the camera, so waiting
    // six frames would mean streaming ground it has already left -- and holes ahead of it.
    expect(latch_stream_centre(state, ChunkCoord{0, 4}, 8) == ChunkCoord({0, 4}),
           "a jump of half the view radius is adopted on the frame it happens");
    expect(latch_stream_centre(state, ChunkCoord{40, 40}, 8) == ChunkCoord({40, 40}),
           "and so is a jump right across the world");

    // Inside half the radius the latch still holds.
    StreamCentre slow;
    latch_stream_centre(slow, ChunkCoord{0, 0}, 8);
    expect(latch_stream_centre(slow, ChunkCoord{0, 3}, 8) == ChunkCoord({0, 0}),
           "a jump of less than half the radius waits like any other candidate");
}

COOPA_TEST(the_collision_radius_is_a_ring_around_the_centre) {
    toy::world::TerrainComponent terrain;
    terrain.collision        = true;
    terrain.collision_radius = 2;
    const ChunkCoord centre{5, 5};

    expect(toy::world::terrain_wants_collision(terrain, centre, centre), "the centre collides");
    expect(toy::world::terrain_wants_collision(terrain, ChunkCoord{7, 3}, centre),
           "a chunk two rings out (diagonally) collides");
    expect(!toy::world::terrain_wants_collision(terrain, ChunkCoord{8, 5}, centre),
           "a chunk three rings out does not");

    terrain.collision_radius = 0;
    expect(toy::world::terrain_wants_collision(terrain, ChunkCoord{99, 99}, centre),
           "radius 0 means every loaded chunk collides");

    terrain.collision = false;
    expect(!toy::world::terrain_wants_collision(terrain, centre, centre),
           "collision off beats any radius");
}
