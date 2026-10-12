/**
 * @file planet_tile_mode_test.cpp
 * @brief Tile Mode on a planet through real input: Add > Planet makes a TileTerrain in planet mode
 *        that bakes a globe, the World Generator previews it unrolled, and Raise strokes dragged
 *        on two of its cube faces lift only that face's tiles under the brush, each one undo step.
 */

#include <coopa/testing/test.h>

#include <toyengine/world/planet.h>
#include <toyengine/world/terrain_component.h>
#include <toyengine/world/tile_map_data.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("planet_tile_mode");

namespace toy::editor::testing {

COOPA_TEST(tile_mode_raises_tiles_on_two_faces_of_a_planet) {
    using coopa::input::MouseButton;
    toy::world::TileMapStore::instance().clear();
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;

    const ObjectId id = app.create_planet();
    {
        auto& doc = app.document();
        const int ci = doc.find_component(id, "TileTerrain");
        coopa::test::require(ci >= 0, "Add > Planet makes a TileTerrain");
        Node comp = doc.find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
        expect(get_bool(comp, "planet", false), "...in planet mode");
        comp["face_tiles"] = Node(static_cast<int64_t>(32));   // a small globe: a quick bake
        doc.set_component(id, ci, comp, "Small planet");
        app.sync().rebuild(engine, doc);
    }
    toy::world::TerrainComponent* t = nullptr;
    for (int i = 0; i < 1200; ++i) {
        tick(engine, 1);
        coopa::scene::SceneObject* live = app.sync().live(id);
        t = live ? live->get_component<toy::world::TerrainComponent>() : nullptr;
        if (t && t->is_ready() && t->tiles && !t->tiles->empty() && t->chunks().size() >= 24) break;
    }
    coopa::test::require(t != nullptr && t->tiles && !t->tiles->empty(), "the planet bakes its globe");
    const std::shared_ptr<toy::world::TileMapData> tiles = t->tiles;
    expect(tiles->width() == 6 * 32 && tiles->height() == 32, "a planet's tiles are six 32 x 32 faces side by side");

    // The World Generator previews the planet's map unrolled (twice as wide as tall).
    app.open_world_generator(id);
    const std::uint64_t serial = app.world_generator_preview_serial();
    for (int i = 0; i < 600 && app.world_generator_preview_serial() == serial; ++i) tick(engine, 1);
    expect(app.world_generator_planet(), "the World Generator opens on a planet as a planet");
    app.close_world_generator();
    tick(engine, 2);

    app.document().select(id);
    app.set_tile_mode(true);
    app.frame_selected();
    tick(engine, 30);
    expect(app.tile_mode(), "Tile Mode entered");

    // The camera's side of the globe: a land tile facing it on the face most squarely toward it,
    // and one on another face still well in view.
    const toy::world::PlanetLayout layout = t->params.layout();
    auto* camera = coopa::gfx::engine::components::CameraComponent::main();
    coopa::test::require(camera != nullptr, "the editor has a camera");
    const glm::vec3 eye = glm::normalize(glm::vec3(glm::inverse(camera->get_view_matrix())[3]));
    auto best_land = [&](int skip_face) {
        glm::ivec2 best{-1};
        float score = 0.35f;
        for (int y = 2; y < layout.n - 2; ++y) {
            for (int x = 0; x < tiles->width(); ++x) {
                const int face = x / layout.n, i = x - face * layout.n;
                if (face == skip_face || i < 2 || i >= layout.n - 2 || tiles->at(x, y).water()) continue;
                const float s = glm::dot(glm::vec3(layout.tile_direction(x, y)), eye);
                if (s > score) { score = s; best = {x, y}; }
            }
        }
        return best;
    };
    const glm::ivec2 a = best_land(-1);
    coopa::test::require(a.x >= 0, "land faces the camera");
    const glm::ivec2 b = best_land(a.x / layout.n);
    coopa::test::require(b.x >= 0, "land on a second face is in view too");

    auto& brush = app.tile_brush();
    brush.tool = TileTool::Raise;
    brush.size = 3;
    brush.strength = 1;
    auto at_tile = [&](const glm::ivec2& tile) {
        const glm::dvec3 d = layout.tile_direction(tile.x, tile.y);
        const double r = layout.radius() + tiles->at(tile.x, tile.y).steps * t->params.height_step;
        return screen_of(engine, app, id, glm::vec3(d * r), in.scale);
    };
    const std::vector<toy::world::TileRecord> before = tiles->records();
    const std::size_t undo0 = app.tile_history().undo_count();
    for (const glm::ivec2 tile : {a, b}) {
        const auto p = at_tile(tile);
        coopa::test::require(p.has_value(), "the tile is on screen");
        in.move(*p);
        in.drag(*p + glm::vec2(1.0f, 0.0f), MouseButton::Left, 4);
        tick(engine, 3);
    }
    dump(engine, "planet_tile_mode_raise");
    expect(app.tile_history().undo_count() == undo0 + 2, "each stroke is one undo step");

    // Every changed tile is on one of the two stroked faces, within the brush of its stroke.
    int on_a = 0, on_b = 0, stray = 0;
    for (int y = 0; y < tiles->height(); ++y) {
        for (int x = 0; x < tiles->width(); ++x) {
            const std::size_t i = static_cast<std::size_t>(y * tiles->width() + x);
            if (tiles->records()[i].steps == before[i].steps) continue;
            const auto near = [&](const glm::ivec2& s) { return x / layout.n == s.x / layout.n && std::abs(x - s.x) <= 2 && std::abs(y - s.y) <= 2; };
            if (near(a)) ++on_a;
            else if (near(b)) ++on_b;
            else if (!before[i].water()) ++stray;
        }
    }
    expect(on_a >= 4 && on_b >= 4 && stray == 0,
           "the strokes raise tiles on both faces and nowhere else (" + std::to_string(on_a) + ", " +
               std::to_string(on_b) + ", stray " + std::to_string(stray) + ")");

    app.undo();
    app.undo();
    tick(engine, 2);
    expect(tiles->records() == before, "undo restores the globe exactly");
}

} // namespace toy::editor::testing
