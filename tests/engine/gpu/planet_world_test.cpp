/**
 * @file planet_world_test.cpp
 * @brief A planet TileTerrain end to end: every chunk of the globe comes up, bodies fall to the
 *        planet from any side, its sea floats a crate, and a character walks on it upright.
 *
 * The scene is the shipped planet_demo shrunk to a 32-tile face (a ~20 m planet) so it bakes
 * and meshes in a second or two; its own falling bodies are dropped -- each test places its own.
 */

#include <algorithm>
#include <filesystem>

#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/testing/test.h>

#include <cmath>
#include <toyengine/core/engine.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_body.h>
#include <toyengine/world/planet.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_map_data.h>
#include <physxcoopa/components/box_collider.h>
#include <physxcoopa/components/rigidbody.h>
#include <physxcoopa/components/sphere_collider.h>
#include <physxcoopa/system/physics_system.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/scene_variant.h"
#include "engine/support/terrain_fixtures.h"

COOPA_TEST_SUITE("planet_world");

using namespace toy::test;

namespace {

/** @brief planet_demo, shrunk, without its dropped bodies. */
std::string small_planet() {
    toy::world::TileMapStore::instance().clear();
    std::string path = scene_variant("assets/scenes/terrain/planet_demo/scene.yaml", "small_planet", [](fkyaml::node& doc) {
        if (fkyaml::node* t = scene_component(doc, "planet", "TileTerrain")) {
            (*t)["face_tiles"] = fkyaml::node(32);
            (*t)["height_scale"] = fkyaml::node(6.0);
        }
        auto& roots = doc["scene"]["root_objects"].as_seq();
        roots.erase(std::remove_if(roots.begin(), roots.end(), [](const fkyaml::node& o) {
                        return o.contains("name") && o.at("name").get_value<std::string>().rfind("drop_", 0) == 0;
                    }),
                    roots.end());
    });
    // The whole `terrain/` directory, not one filename: a stored tile map is named after its
    // object (a planet's is `planet.tiles`), and one left from another face_tiles would be
    // loaded instead of this variant's map ever being baked.
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(path).parent_path() / "terrain", ec);
    return path;
}

struct Planet {
    coopa::scene::Scene* scene = nullptr;
    toy::world::TerrainComponent* terrain = nullptr;
    coopa::physx::system::PhysicsSystem* physics = nullptr;
};

/** @brief Starts the planet and runs until every chunk of it is live. */
Planet bring_up(toy::core::Engine& engine) {
    Planet p;
    p.scene = &engine.scene();
    p.terrain = p.scene->find_first_component<toy::world::TerrainComponent>();
    p.physics = dynamic_cast<coopa::physx::system::PhysicsSystem*>(p.scene->find_system("Physics"));
    if (p.terrain == nullptr) return p;
    const int n = p.terrain->params.face_tiles / p.terrain->params.chunk_size;
    tick_until(engine, 1500, [&] { return count_live_chunks(*p.terrain) >= 6 * n * n; });
    return p;
}

/** @brief A tile of the kind `want` accepts, nearest the direction `toward`: its centre direction. */
template <typename Want>
glm::vec3 find_tile(const toy::world::TerrainComponent& terrain, const glm::vec3& toward, Want want) {
    const toy::world::PlanetLayout layout = terrain.params.layout();
    const toy::world::TileMapData& tiles = *terrain.tiles;
    float best = -2.0f;
    glm::vec3 dir(0.0f);
    for (int y = 0; y < tiles.height(); ++y) {
        for (int x = 0; x < tiles.width(); ++x) {
            if (!want(tiles, x, y)) continue;
            const glm::vec3 d(layout.tile_direction(x, y));
            if (glm::dot(d, toward) > best) {
                best = glm::dot(d, toward);
                dir = d;
            }
        }
    }
    return dir;
}

coopa::scene::SceneObject* spawn(coopa::scene::Scene& scene, std::unique_ptr<coopa::scene::SceneObject> object,
                                 coopa::physx::system::PhysicsSystem* physics) {
    coopa::scene::SceneObject* raw = object.get();
    scene.add_root_object(std::move(object));
    scene.adopt(*raw);
    if (physics) physics->refresh();
    return raw;
}

glm::vec3 position_of(const coopa::scene::SceneObject* o) {
    return glm::vec3(o->get_transform()->transform().get_world_matrix()[3]);
}

} // namespace

/**
 * @brief Every chunk of the globe comes up, and balls dropped beside it on opposite sides each
 *        fall to the planet -- toward its centre, not down world Z -- and come to rest on it.
 */
COOPA_TEST(bodies_fall_to_the_planet_from_any_side) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config(small_planet(), 320, 180, 160, 90));
    Planet p = bring_up(engine);
    require(p.terrain != nullptr && p.physics != nullptr, "planet: the scene has a terrain and physics");
    const int n = p.terrain->params.face_tiles / p.terrain->params.chunk_size;
    expect(count_live_chunks(*p.terrain) == 6 * n * n,
           "planet: all " + std::to_string(6 * n * n) + " chunks are live (" + std::to_string(count_live_chunks(*p.terrain)) + ")");
    expect(p.physics->world().gravity_source_count() == 1, "planet: the planet registered its gravity");

    const float radius = p.terrain->params.layout().radius();
    const float top = radius + p.terrain->params.height_scale + 4.0f;
    std::vector<coopa::scene::SceneObject*> balls;
    for (const glm::vec3 side : {glm::vec3(0, 0, -1), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0)}) {
        auto ball = std::make_unique<coopa::scene::SceneObject>("ball");
        ball->add_component<coopa::scene::TransformComponent>()->transform().set_position(side * top);
        ball->add_component<coopa::physx::components::SphereCollider>()->set_radius(0.5f);
        ball->add_component<coopa::physx::components::RigidbodyComponent>()->mass = 5.0f;
        balls.push_back(spawn(*p.scene, std::move(ball), p.physics));
    }
    tick_frames(engine, 480);
    const glm::vec3 sides[3] = {glm::vec3(0, 0, -1), glm::vec3(1, 0, 0), glm::vec3(0, -1, 0)};
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 at = position_of(balls[static_cast<std::size_t>(i)]);
        const float r = glm::length(at);
        const float along = glm::dot(glm::normalize(at), sides[i]);
        expect(r < top - 1.0f && r > radius - 3.0f && along > 0.8f,
               "planet: a ball dropped beside side " + std::to_string(i) + " lands on it (r " + std::to_string(r) +
                   ", radius " + std::to_string(radius) + ", along " + std::to_string(along) + ")");
        auto* body = p.physics->world().get_body(balls[static_cast<std::size_t>(i)]
                                                     ->get_component<coopa::physx::components::RigidbodyComponent>()->body_id());
        // Measured along the RADIAL direction, not as total speed. "Came to rest on the planet"
        // is a claim about falling having stopped -- the surface supports the ball instead of it
        // sinking through or bouncing off -- and radial velocity is exactly that quantity. Total
        // speed is not: a sphere resting on a bumpy globe keeps rolling TANGENTIALLY for a long
        // time, since every spot that is not a local minimum is a slope under centre-pointing
        // gravity. Ball 0 here does settle to exactly 0, so nothing is injecting energy; the
        // other two simply land on slopes and roll, decaying over thousands of frames. Which
        // side of a 1 m/s total-speed line that roll happened to be on at this particular frame
        // was luck, and is what made this check fail without anything being wrong.
        const float radial = body ? std::abs(glm::dot(body->linear_velocity, glm::normalize(at))) : -1.0f;
        expect(body != nullptr && radial < 1.0f,
               "planet: ...and comes to rest on it (radial " + std::to_string(radial) + " m/s)");
        // ...and is still on the planet rather than launched off it.
        expect(body != nullptr && glm::length(body->linear_velocity) < 4.0f,
               "planet: ...without being flung off (" + std::to_string(body ? glm::length(body->linear_velocity) : -1.0f) + " m/s)");
    }
}

/**
 * @brief The planet's sea is curved water in each chunk's own frame, and a buoyant crate dropped
 *        into it floats at its surface -- buoyancy pushing away from the planet, not up world Z.
 */
COOPA_TEST(the_sea_floats_a_crate) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config(small_planet(), 320, 180, 160, 90));
    Planet p = bring_up(engine);
    require(p.terrain != nullptr && p.terrain->tiles != nullptr, "sea: the planet baked");

    int frame_bodies = 0;
    for (const auto* body : p.scene->get_components<toy::water::WaterBody>()) frame_bodies += body->local_frame ? 1 : 0;
    expect(frame_bodies > 0, "sea: the planet's water bodies are local-frame bodies (" + std::to_string(frame_bodies) + ")");

    // Deep, open sea: still water all round, at least two steps deep.
    const toy::world::PlanetLayout layout = p.terrain->params.layout();
    const glm::vec3 dir = find_tile(*p.terrain, glm::vec3(0, 0, 1), [&](const toy::world::TileMapData& t, int x, int y) {
        const toy::world::TileRecord& r = t.at(x, y);
        if (!r.has(toy::world::TileRecord::Ocean) || r.water_depth < 2) return false;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const glm::ivec2 nb = layout.neighbour(x, y, dx, dy);
                if (!t.at(nb.x, nb.y).water() || t.at(nb.x, nb.y).has(toy::world::TileRecord::Flowing)) return false;
            }
        return true;
    });
    require(glm::length(dir) > 0.5f, "sea: the planet has open sea");
    const glm::ivec2 tile = layout.tile_of(glm::dvec3(dir));
    const toy::world::TileRecord& r = p.terrain->tiles->at(tile.x, tile.y);
    const float surface = layout.radius() + (static_cast<float>(r.water_level()) - p.terrain->params.water_inset) * p.terrain->params.height_step;
    const float bed = layout.radius() + static_cast<float>(r.steps) * p.terrain->params.height_step;

    auto crate = std::make_unique<coopa::scene::SceneObject>("float_crate");
    crate->add_component<coopa::scene::TransformComponent>()->transform().set_position(dir * (surface + 2.0f));
    crate->add_component<coopa::physx::components::BoxCollider>()->set_size(glm::vec3(0.8f));
    crate->add_component<coopa::physx::components::RigidbodyComponent>()->mass = 150.0f;
    auto* buoyancy = crate->add_component<toy::water::Buoyancy>();
    coopa::scene::SceneObject* raw = spawn(*p.scene, std::move(crate), p.physics);
    tick_frames(engine, 480);
    const float at = glm::length(position_of(raw));
    expect(buoyancy->in_water, "sea: the crate is in the water");
    expect(at > bed + 0.6f && std::abs(at - surface) < 0.6f,
           "sea: the crate floats at the surface (r " + std::to_string(at) + ", surface " + std::to_string(surface) +
               ", bed " + std::to_string(bed) + ")");
}

/**
 * @brief The player, dropped onto land, stands on the planet upright (its up away from the
 *        centre) and, walking forward, follows the curve of the ground rather than leaving it.
 */
COOPA_TEST(a_character_walks_on_the_planet) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config(small_planet(), 320, 180, 160, 90));
    Planet p = bring_up(engine);
    require(p.terrain != nullptr && p.terrain->tiles != nullptr, "walk: the planet baked");
    auto* player = p.scene->find_first_component<toy::scene::CharacterController>();
    require(player != nullptr && player->owner != nullptr, "walk: the scene has a player");

    // Dry, fairly level land: the tile and its neighbours within a step of one another.
    const toy::world::PlanetLayout layout = p.terrain->params.layout();
    const glm::vec3 dir = find_tile(*p.terrain, glm::vec3(0, 0, 1), [&](const toy::world::TileMapData& t, int x, int y) {
        const toy::world::TileRecord& r = t.at(x, y);
        if (r.water()) return false;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const glm::ivec2 nb = layout.neighbour(x, y, dx, dy);
                const toy::world::TileRecord& o = t.at(nb.x, nb.y);
                if (o.water() || std::abs(static_cast<int>(o.steps) - static_cast<int>(r.steps)) > 1) return false;
            }
        return true;
    });
    require(glm::length(dir) > 0.5f, "walk: the planet has level land");
    const float top = layout.radius() + p.terrain->params.height_scale + 2.0f;
    player->teleport(dir * top);
    tick_frames(engine, 180);
    expect(player->is_grounded(), "walk: the player lands on the planet");

    // Driven here, not by the (absent) input.
    player->player_input = false;
    const glm::vec3 start = position_of(player->owner);
    for (int i = 0; i < 120; ++i) {
        player->move_input = glm::vec2(0.0f, 1.0f);
        engine.tick();
    }
    player->move_input = glm::vec2(0.0f);
    tick_frames(engine, 30);
    const glm::vec3 end = position_of(player->owner);
    const glm::vec3 up = glm::normalize(glm::vec3(player->owner->get_transform()->transform().get_world_matrix()[2]));
    const float travelled = glm::distance(start, end);
    const float height = glm::length(end) - layout.radius();
    expect(travelled > 2.0f, "walk: the player moved (" + std::to_string(travelled) + " m)");
    expect(height > -1.0f && height < p.terrain->params.height_scale + 2.0f,
           "walk: ...staying on the ground (altitude " + std::to_string(height) + ")");
    expect(glm::dot(up, glm::normalize(end)) > 0.95f,
           "walk: ...upright, its up away from the planet (" + std::to_string(glm::dot(up, glm::normalize(end))) + ")");
}

