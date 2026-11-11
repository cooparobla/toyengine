/**
 * @file planet_atmosphere_test.cpp
 * @brief PlanetAtmosphere sizes a sibling Volume from the nearest ancestor planet terrain's
 *        actual measured peak height, re-firing exactly when that height changes.
 *
 * Also planet_cloud_deck(), which sizes a planet's volumetric cloud layer from the same
 * measurement -- the render config's absolute cloud_altitude/cloud_thickness/cloud_scale
 * being meaningless on a globe a few hundred units across, and discarded there.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

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

// --- The cloud deck -------------------------------------------------------------------------
//
// planet_cloud_deck() is what a planet scene's cloud layer is actually sized by: the render
// config's cloud_altitude/cloud_thickness/cloud_scale are absolute metres tuned for an
// Earth-sized sky and are discarded. These lock the arithmetic down here, with no device, so
// a change to it fails in four seconds rather than in a GPU capture.

COOPA_TEST(the_cloud_deck_clears_the_measured_peak) {
    Fixture f(40);
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    const toy::world::CloudDeck deck =
        toy::world::planet_cloud_deck(f.scene, toy::env::PlanetFrame{glm::vec3(0.0f), radius});

    expect_near(deck.altitude, 40.0f * 1.05f, 1e-3f,
                "the deck's base sits just clear of the tallest mountain");
    expect(deck.altitude > 40.0f, "which is above the peak, not level with it");
}

COOPA_TEST(the_cloud_deck_has_a_radius_keyed_floor) {
    // A world with no relief at all: the peak term is 0, so only the radius floor can lift the
    // deck off the sea. Without it an ocean world's clouds would lie on the water.
    Fixture f(0, 512);
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    const toy::world::CloudDeck deck =
        toy::world::planet_cloud_deck(f.scene, toy::env::PlanetFrame{glm::vec3(0.0f), radius});

    expect_near(deck.altitude, radius * 0.01f, 1e-3f, "a flat world's deck is keyed to the radius");
    expect(deck.altitude > 0.0f, "so it is still off the ground");
    expect_near(deck.thickness, radius * 0.04f, 1e-3f,
                "and its depth is radius-keyed too, so it reads as a layer from orbit");
}

COOPA_TEST(the_cloud_deck_scale_is_three_weather_periods_around_the_globe) {
    // The one derived value that is not obvious. The cloud field's noise periods are FIXED
    // cloud-space lengths (13 km), so at scale 1 a globe whose whole circumference is under a
    // kilometre of cloud space reads one constant weather value everywhere and comes out
    // featureless. Three periods per revolution is what gives it distinct fronts.
    Fixture f(40, 512);
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    const toy::world::CloudDeck deck =
        toy::world::planet_cloud_deck(f.scene, toy::env::PlanetFrame{glm::vec3(0.0f), radius});

    const float deck_radius = radius + deck.altitude + deck.thickness * 0.5f;
    expect_near(deck.scale, 2.0f * glm::pi<float>() * deck_radius / (3.0f * 13000.0f), 1e-6f,
                "the field is scaled to three weather periods around the deck's own circumference");
    expect(deck.scale < 1.0f, "which on a toy globe is far below the authored default of 1");
}

COOPA_TEST(a_scene_with_no_planet_atmosphere_still_derives_a_deck) {
    // A hand-written `planet: true` + `clouds: true` scene has no PlanetAtmosphere at all, and
    // must still get a working layer rather than the config's Earth-sized metres.
    Scene bare{"planet_without_atmosphere"};
    auto planet = std::make_unique<SceneObject>("planet");
    planet->add_component<TransformComponent>();
    TerrainComponent* terrain = planet->add_component<TerrainComponent>();
    terrain->params.planet      = true;
    terrain->params.face_tiles  = 16;
    terrain->params.tile_size   = 1.0f;
    terrain->params.chunk_size  = 8;
    terrain->params.height_step = 1.0f;
    toy::world::TerraGeometry g;
    g.tiles_x     = 6 * terrain->params.face_tiles;
    g.tiles_y     = terrain->params.face_tiles;
    g.tile_size   = terrain->params.tile_size;
    g.height_step = terrain->params.height_step;
    g.chunk_tiles = terrain->params.chunk_size;
    terrain->field.reset(g);
    terrain->field.data()->at(3, 3).steps = 40;
    terrain->field.mark_all_dirty();
    bare.add_root_object(std::move(planet));
    bare.start();

    const float radius = terrain->params.layout().radius();
    const toy::world::CloudDeck deck =
        toy::world::planet_cloud_deck(bare, toy::env::PlanetFrame{glm::vec3(0.0f), radius});

    expect_near(deck.altitude, 40.0f * 1.05f, 1e-3f, "the deck is derived from the terrain alone");
    expect(deck.thickness > 0.0f && deck.scale > 0.0f, "and is a usable layer, not a degenerate one");
}

COOPA_TEST(cloud_deck_defaults_reproduce_the_derived_deck) {
    // The no-existing-scene-changes guarantee, asserted rather than claimed: a PlanetAtmosphere
    // whose four cloud knobs are untouched must give numerically the same deck as a scene with
    // no PlanetAtmosphere at all, which is the derivation as it stood before they existed.
    Fixture f(40, 64);
    f.scene.update(1.0f / 60.0f);
    const float radius = f.terrain->params.layout().radius();
    const toy::env::PlanetFrame frame{glm::vec3(0.0f), radius};

    const toy::world::CloudDeck authored = toy::world::planet_cloud_deck(f.scene, frame);
    expect_near(authored.altitude, std::max(40.0f * 1.05f, radius * 0.01f), 1e-3f,
                "the default knobs are the old expression, term for term");
    expect_near(authored.thickness, std::max(radius * 0.04f, authored.altitude * 0.5f), 1e-3f,
                "including the depth");
}

COOPA_TEST(cloud_knobs_raise_the_deck) {
    Fixture f(40, 64);
    // Generous air, so the inside-the-shell clamp cannot be what is being measured here. The
    // deck asks for altitude + thickness, and thickness is half the altitude again, so the
    // shell has to clear 1.5x the altitude -- 2.0 here is NOT enough, which this test found.
    f.sizer->radius_thickness_scale = 6.0f;
    f.sizer->cloud_altitude_peak_scale = 2.5f;
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    const toy::world::CloudDeck deck =
        toy::world::planet_cloud_deck(f.scene, toy::env::PlanetFrame{glm::vec3(0.0f), radius});

    expect_near(deck.altitude, 40.0f * 2.5f, 1e-3f, "the peak-keyed knob lifts the deck");
    expect(deck.altitude > 40.0f * 1.05f, "well above where the default put it");
}

COOPA_TEST(the_radius_knob_raises_a_low_relief_decks_floor) {
    Fixture f(0, 64);   // no relief: only the radius term can lift it
    f.sizer->radius_thickness_scale = 2.0f;
    f.sizer->cloud_altitude_radius_scale = 0.10f;
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    const toy::world::CloudDeck deck =
        toy::world::planet_cloud_deck(f.scene, toy::env::PlanetFrame{glm::vec3(0.0f), radius});

    expect_near(deck.altitude, radius * 0.10f, 1e-3f, "the radius-keyed knob lifts a flat world's deck");
}

COOPA_TEST(the_cloud_deck_stays_inside_the_atmosphere_shell) {
    // A deck outside the air is cloud in vacuum, and sits beyond the volumetric march's own ray
    // bound (the shell's outer radius), so it would lose all of the shell's in-scatter and read
    // as a layer in front of the limb instead of within it.
    Fixture f(40, 64);
    f.sizer->cloud_altitude_peak_scale = 50.0f;   // absurd on purpose
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    const toy::world::CloudDeck deck =
        toy::world::planet_cloud_deck(f.scene, toy::env::PlanetFrame{glm::vec3(0.0f), radius});

    expect(f.sizer->shell_outer() > f.sizer->shell_inner(), "the shell has been measured");
    expect(radius + deck.altitude + deck.thickness <= f.sizer->shell_outer() + 1e-2f,
           "the deck's top is squeezed back inside the air it is weather in");
    expect(deck.thickness > 0.0f, "and is still a layer, not collapsed to nothing");
}

COOPA_TEST(an_edited_cloud_knob_re_applies) {
    // The knobs join apply()'s settings hash: without that, an inspector drag would do nothing
    // until the terrain's height happened to change.
    Fixture f(40, 64);
    f.scene.update(1.0f / 60.0f);
    const float before = f.volume->extent.x;

    f.sizer->cloud_thickness_radius_scale = 0.5f;
    f.scene.update(1.0f / 60.0f);

    expect_near(f.volume->extent.x, before, 1e-3f,
                "a cloud knob does not move the shell -- it is the deck's, not the air's");
    const float radius = f.terrain->params.layout().radius();
    const toy::world::CloudDeck deck =
        toy::world::planet_cloud_deck(f.scene, toy::env::PlanetFrame{glm::vec3(0.0f), radius});
    expect(deck.thickness > std::max(radius * 0.04f, deck.altitude * 0.5f) - 1e-3f,
           "but it does reach the deck without waiting for a re-bake");
}

COOPA_TEST(limb_optical_depth_derives_the_density) {
    Fixture f(40, 64);
    f.sizer->limb_optical_depth = 2.0f;
    f.scene.update(1.0f / 60.0f);

    const float radius = f.terrain->params.layout().radius();
    const float grazing = std::sqrt(2.0f * glm::pi<float>() * f.volume->height_falloff * radius);
    expect_near(f.volume->density, 2.0f / grazing, 1e-6f,
                "density is the depth divided by the grazing-path integral through the air");
    expect(f.volume->density > 0.0f, "and is a usable density, not zero");
}

COOPA_TEST(limb_optical_depth_zero_leaves_the_volumes_density_alone) {
    Fixture f(40, 64);
    f.volume->density = 0.0123f;          // whatever the scene authored
    f.scene.update(1.0f / 60.0f);
    expect_near(f.volume->density, 0.0123f, 1e-9f,
                "the default of 0 is opt-in: an authored per-metre density is untouched");
}

COOPA_TEST(the_same_limb_depth_looks_the_same_on_any_sized_planet) {
    // The whole point. Volume::density is per unit of LENGTH, so an authored one silently
    // changes what it means as the globe grows -- which is how planet_demo's atmosphere went
    // from a thin veil to an invisible one when its radius doubled. Keyed to a DEPTH, the two
    // globes agree.
    const auto depth_of = [](std::int32_t face_tiles, std::int32_t peak) {
        Fixture f(peak, face_tiles);
        f.sizer->limb_optical_depth = 2.0f;
        f.scene.update(1.0f / 60.0f);
        const float radius = f.terrain->params.layout().radius();
        // Re-derive the grazing depth from what the component actually wrote.
        return f.volume->density * std::sqrt(2.0f * glm::pi<float>() * f.volume->height_falloff * radius);
    };

    const float small = depth_of(64, 20);
    const float large = depth_of(512, 160);
    expect_near(small, 2.0f, 1e-3f, "a small globe reaches the authored limb depth");
    expect_near(large, 2.0f, 1e-3f, "and so does one eight times its radius, from the same number");
}
