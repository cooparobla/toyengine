/**
 * @file terrain_chunk_query_test.cpp
 * @brief TerrainComponent::chunk_state_at() / chunk_loaded_at(): which chunk a world position
 *        falls on, and whether that ground is streamed in.
 *
 * What gameplay content left lying on the ground keys its lifetime to -- a corpse, a dropped
 * item, a stamped camp -- so the answer has to be right at a negative coordinate (floor, not
 * truncate, or everything in the -x/-y quadrant reads one chunk too high), under the terrain
 * object's own Transform, and for a chunk the streamer has parked or let go of as much as for
 * one that is drawing.
 */

#include <coopa/testing/test.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <toyengine/world/terrain_component.h>

#include <memory>

COOPA_TEST_SUITE("terrain_chunk_query");

using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
using toy::world::ChunkCoord;
using toy::world::ChunkState;
using toy::world::TerrainComponent;

namespace {

/** @brief A terrain of 8x8-tile chunks of 2 m tiles (16 m a chunk), at `at`, with no tiles. */
struct Fixture {
    coopa::scene::Scene scene{"terrain_chunk_query"};
    TerrainComponent* terrain = nullptr;

    explicit Fixture(const glm::vec3& at = glm::vec3(0.0f)) {
        auto obj = std::make_unique<SceneObject>("terrain");
        obj->add_component<TransformComponent>()->transform().set_position(at);
        terrain = obj->add_component<TerrainComponent>();
        terrain->params.chunk_size = 8;
        terrain->params.tile_size = 2.0f;
        scene.add_root_object(std::move(obj));
    }

    /** @brief Puts a chunk in the table at `coord`, in `state`. */
    void add_chunk(ChunkCoord coord, ChunkState state) {
        toy::world::TerrainChunk chunk;
        chunk.coord = coord;
        chunk.state = state;
        terrain->chunks().emplace(coord, std::move(chunk));
    }
};

}  // namespace

/** @brief A position lands on the chunk that covers it, at either sign. */
COOPA_TEST(a_position_finds_the_chunk_under_it) {
    Fixture f;
    f.add_chunk({0, 0}, ChunkState::Live);
    f.add_chunk({1, 0}, ChunkState::Live);
    f.add_chunk({-1, -1}, ChunkState::Live);

    // Chunk (0,0) spans [0,16) on both axes; z is ignored.
    ASSERT_TRUE(f.terrain->chunk_loaded_at(glm::vec3(0.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(f.terrain->chunk_loaded_at(glm::vec3(15.9f, 0.1f, 400.0f)));
    ASSERT_TRUE(f.terrain->chunk_loaded_at(glm::vec3(16.1f, 7.0f, -400.0f)));   // (1,0)
    // Floored, not truncated: everything in [-16,0) is chunk -1, including -0.1.
    ASSERT_TRUE(f.terrain->chunk_loaded_at(glm::vec3(-0.1f, -0.1f, 0.0f)));
    ASSERT_TRUE(f.terrain->chunk_loaded_at(glm::vec3(-15.9f, -15.9f, 0.0f)));
    // And nothing is claimed where no chunk is.
    ASSERT_FALSE(f.terrain->chunk_state_at(glm::vec3(100.0f, 100.0f, 0.0f)).has_value());
    ASSERT_FALSE(f.terrain->chunk_loaded_at(glm::vec3(-16.1f, -0.1f, 0.0f)));   // (-2,-1)
    ASSERT_FALSE(f.terrain->chunk_loaded_at(glm::vec3(0.0f, 20.0f, 0.0f)));     // (0,1)
}

/**
 * @brief Loaded is Queued, Meshing or Live -- a Live chunk being rebuilt passes through Meshing
 *        while it is still drawing -- and NOT Dormant or Retiring, which have no ground left.
 */
COOPA_TEST(only_a_streamed_in_chunk_counts_as_loaded) {
    Fixture f;
    f.add_chunk({0, 0}, ChunkState::Live);
    f.add_chunk({1, 0}, ChunkState::Meshing);
    f.add_chunk({2, 0}, ChunkState::Queued);
    f.add_chunk({3, 0}, ChunkState::Dormant);
    f.add_chunk({4, 0}, ChunkState::Retiring);

    const auto at = [&](int chunk_x) { return glm::vec3(chunk_x * 16.0f + 1.0f, 1.0f, 0.0f); };
    ASSERT_TRUE(f.terrain->chunk_loaded_at(at(0)));
    ASSERT_TRUE(f.terrain->chunk_loaded_at(at(1)));
    ASSERT_TRUE(f.terrain->chunk_loaded_at(at(2)));
    ASSERT_FALSE(f.terrain->chunk_loaded_at(at(3)));
    ASSERT_FALSE(f.terrain->chunk_loaded_at(at(4)));
    // The state itself is readable, for a caller that wants to tell parked from gone.
    ASSERT_TRUE(f.terrain->chunk_state_at(at(3)) == ChunkState::Dormant);
}

/** @brief The query is in world space: the terrain's own Transform is taken off first. */
COOPA_TEST(the_terrains_transform_is_accounted_for) {
    Fixture f(glm::vec3(1000.0f, -500.0f, 12.0f));
    f.add_chunk({0, 0}, ChunkState::Live);

    ASSERT_TRUE(f.terrain->chunk_loaded_at(glm::vec3(1001.0f, -499.0f, 0.0f)));
    ASSERT_FALSE(f.terrain->chunk_loaded_at(glm::vec3(1.0f, 1.0f, 0.0f)));
}
