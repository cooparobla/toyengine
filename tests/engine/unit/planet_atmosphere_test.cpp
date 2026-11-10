/**
 * @file planet_atmosphere_test.cpp
 * @brief PlanetAtmosphere sizes a sibling Volume from the nearest ancestor planet terrain's
 *        actual measured peak height, re-firing exactly when that height changes.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <memory>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <gfxcoopa/engine/components/volume.h>
#include <toyengine/world/planet_atmosphere.h>
#include <toyengine/world/terrain_component.h>

COOPA_TEST_SUITE("planet_atmosphere");

using coopa::gfx::engine::components::VolumeComponent;
using coopa::gfx::engine::components::VolumeShape;
using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
using toy::world::PlanetAtmosphere;
using toy::world::TerrainComponent;
using coopa::test::expect;
using coopa::test::expect_near;

namespace {

/**
 * @brief A planet terrain (default face_tiles 16, tile_size 1 -> radius ~10.186) with one tile
 *        raised to `peak_steps`, and an "atmosphere" child carrying a Volume +
 *        PlanetAtmosphere. A larger `face_tiles` is a larger globe: radius = 2 N / pi.
 */
struct Fixture {
    Scene scene{"planet_atmosphere_test"};
    TerrainComponent* terrain  = nullptr;
    VolumeComponent*  volume   = nullptr;
    PlanetAtmosphere* sizer    = nullptr;

    explicit Fixture(std::int32_t peak_steps, std::int32_t face_tiles = 16) {
        auto planet = std::make_unique<SceneObject>("planet");
        planet->add_component<TransformComponent>();
        terrain = planet->add_component<TerrainComponent>();
        terrain->params.planet     = true;
        terrain->params.face_tiles = face_tiles;
        terrain->params.tile_size  = 1.0f;
        terrain->params.chunk_size = 8;
        terrain->params.height_step = 1.0f;

        toy::world::TerraGeometry g;
        g.tiles_x     = 6 * terrain->params.face_tiles;  // six faces side by side
        g.tiles_y     = terrain->params.face_tiles;
        g.tile_size   = terrain->params.tile_size;
        g.height_step = terrain->params.height_step;
        g.chunk_tiles = terrain->params.chunk_size;
        terrain->field.reset(g);
        terrain->field.data()->at(3, 3).steps = peak_steps;
        terrain->field.mark_all_dirty();

        auto atmosphere = std::make_unique<SceneObject>("atmosphere");
        atmosphere->add_component<TransformComponent>();
        volume = atmosphere->add_component<VolumeComponent>();
        volume->shape = VolumeShape::Shell;
        sizer  = atmosphere->add_component<PlanetAtmosphere>();
        sizer->thickness_scale        = 1.8f;
        sizer->min_thickness          = 30.0f;
        sizer->scale_height_fraction  = 0.45f;

        planet->add_child(std::move(atmosphere));
        scene.add_root_object(std::move(planet));
        toy::world::install_planet_atmosphere_system(scene);
        scene.start();
    }

    /** @brief The sky height the fixture's `peak` implies. */
    static float thickness_for(float peak) { return std::max(30.0f, peak * 1.8f); }
};

} // namespace

COOPA_TEST(sizes_the_shell_from_the_terrains_measured_peak) {
    Fixture f(5);   // one tile at 5 steps * 1 m = 5 m above the planet's nominal radius
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    const float thickness = Fixture::thickness_for(5.0f);   // 5 * 1.8 < 30, so the floor wins

    expect_near(thickness, 30.0f, 1e-3f, "a near-flat world falls back to min_thickness");
    expect_near(f.volume->extent.x, radius + thickness, 1e-3f,
                "the shell's outer radius is its base plus the sky's thickness");
    expect_near(f.volume->height_base, radius, 1e-3f,
                "the shell's inner radius is the planet's own nominal datum");
    expect_near(f.volume->height_falloff, thickness * 0.45f, 1e-3f,
                "the scale height is scale_height_fraction of the thickness");
}

COOPA_TEST(a_tall_world_gets_a_proportionally_taller_sky) {
    Fixture f(40);   // 40 * 1.8 = 72, well past min_thickness
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    expect_near(Fixture::thickness_for(40.0f), 72.0f, 1e-3f, "thickness scales with the peak");
    expect_near(f.volume->extent.x, radius + 72.0f, 1e-3f,
                "mountains 8x taller than the flat case get far more air above them, not a fixed margin");
    expect_near(f.volume->height_falloff, 72.0f * 0.45f, 1e-3f,
                "the scale height grows with the sky, so the air is never a dense skin at the datum");
}

COOPA_TEST(base_altitude_scale_floats_the_shell_clear_of_the_peak) {
    Fixture f(40);
    f.sizer->base_altitude_scale = 0.9f;      // a cloud deck, not an atmosphere
    f.terrain->field.mark_all_dirty();        // re-fire with the new setting
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    const float base   = radius + 40.0f * 0.9f;
    expect_near(f.volume->height_base, base, 1e-3f,
                "the shell starts above the terrain instead of on the datum");
    expect_near(f.volume->extent.x, base + Fixture::thickness_for(40.0f), 1e-3f,
                "and its outer radius follows that raised base");
}

COOPA_TEST(a_second_update_with_no_new_bake_is_a_no_op) {
    Fixture f(5);
    f.scene.update(1.0f / 60.0f);
    const float first_outer = f.volume->extent.x;

    // Corrupt the field the component owns; an unchanged terrain must not touch it again.
    f.volume->extent.x = -1.0f;
    f.scene.update(1.0f / 60.0f);
    expect(f.volume->extent.x == -1.0f,
           "no re-bake since the last update: the component skips the recompute");

    // A re-bake (mark_all_dirty bumps the height revision) makes it re-fire.
    f.terrain->field.mark_all_dirty();
    f.scene.update(1.0f / 60.0f);
    expect_near(f.volume->extent.x, first_outer, 1e-3f,
                "a fresh bake re-fires the recompute, back to the correct value");
}

COOPA_TEST(a_taller_peak_grows_the_shell) {
    Fixture low(5);
    low.scene.update(1.0f / 60.0f);
    Fixture high(40);
    high.scene.update(1.0f / 60.0f);

    expect(high.volume->extent.x > low.volume->extent.x,
           "a taller measured peak pushes the shell's outer radius further out");
}

// --- Sizing from the planet's radius, so a world of any size gets a working sky ---

COOPA_TEST(a_large_planet_gets_a_radius_proportional_sky) {
    // A big globe whose relief is trivial: at face_tiles 512 the radius is ~326, so the
    // radius term (326 * 0.12 = 39) beats both the peak term (5 * 1.8 = 9) and the 30-unit
    // floor. Keyed to the peak alone this planet would wear a 30-unit skin -- 4.6% of its
    // radius -- which is the bug: an atmosphere has to scale with the world it wraps.
    Fixture f(5, 512);
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    const float expected = radius * 0.12f;
    expect(expected > 30.0f && expected > 5.0f * 1.8f,
           "on this globe the radius term is the one that should win");
    expect_near(f.volume->extent.x - f.volume->height_base, expected, 1e-2f,
                "the sky's thickness is a fraction of the planet's radius");
    expect_near(f.volume->height_base, radius, 1e-2f,
                "its base is still the datum (radius_base_scale defaults to 0)");
    expect_near(f.volume->height_falloff, expected * 0.45f, 1e-2f,
                "and the scale height follows that thickness");
}

COOPA_TEST(relief_still_wins_where_the_terrain_is_the_taller_term) {
    // The guarantee that the radius term is a FLOOR, not a replacement: a small globe with
    // real mountains keeps its peak-proportional sky, so every scene tuned before this
    // renders exactly as it did.
    Fixture f(40);
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    expect(radius * 0.12f < 40.0f * 1.8f, "the peak term is the larger one here");
    expect_near(f.volume->extent.x - f.volume->height_base, 72.0f, 1e-3f,
                "a mountainous world is still sized from its measured peak");
}

COOPA_TEST(a_knob_edit_re_applies_without_a_re_bake) {
    // The profile depends on the knobs as well as the terrain, so an inspector drag has to
    // take effect; keyed to the height revision alone it would silently do nothing.
    Fixture f(5);
    f.scene.update(1.0f / 60.0f);
    const float before = f.volume->extent.x;

    f.sizer->radius_thickness_scale = 4.0f;   // 10.186 * 4 = 40.7, past the 30-unit floor
    f.scene.update(1.0f / 60.0f);             // no mark_all_dirty(): the bake is untouched

    const float radius = f.terrain->params.layout().radius();
    expect(f.volume->extent.x > before, "the edited knob grew the shell");
    expect_near(f.volume->extent.x - f.volume->height_base, radius * 4.0f, 1e-2f,
                "to exactly the new radius-proportional thickness");
}

COOPA_TEST(radius_base_scale_lifts_a_deck_off_a_low_relief_planet) {
    // What a cloud deck on an ocean world needs: base_altitude_scale is keyed to the peak, so
    // with no relief to speak of it would leave the deck lying on the sea.
    Fixture f(1, 256);
    f.sizer->radius_base_scale = 0.05f;
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    expect_near(f.volume->height_base, radius + radius * 0.05f, 1e-2f,
                "the shell's base is lifted by a fraction of the radius");
    expect(f.volume->height_base > radius + 1.0f,
           "and so clears the 1-unit peak the peak-keyed term would have ignored");
}
