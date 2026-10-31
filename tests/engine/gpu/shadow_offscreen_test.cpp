/**
 * @file shadow_offscreen_test.cpp
 * @brief An off-screen caster's shadow must still land on the visible floor: regression for
 *        the cascade-reach and instanced-batch-cull fixes in pipeline_shadows.cpp and
 *        engine_render_sync.cpp.
 *
 * Both fixture scenes put a 10 m caster far outside the camera's left frustum edge, under a
 * 30 degree sun travelling along +X, so the caster's shadow sweeps the floor from directly
 * under it (off screen) out to where its top's ray lands (x = 0, dead centre of the camera's
 * view at y = -5). One capture, compared against itself across the shadow's own boundary, needs
 * no shadows_enabled toggle (which this engine does not wire through to a live config at all --
 * see make_render_config_() in engine.cpp if that ever gets fixed, this test does not need it).
 *
 * render_scene() also lowers shadow_distance to 12 -- short of the ~20 the caster's height
 * actually needs at 30 degrees, but enough once elevation-scaled (12 / sin 30 = 24) -- so the
 * fix under test (the elevation-scaled reach replacing a flat shadow_distance) is what makes the
 * caster reach the cascade at all, rather than the fixture happening to fit under the shipped
 * default of 60 regardless of the fix. 30 degrees, not a more dramatic low angle, is a deliberate
 * floor: below roughly 15 degrees this scene's direct-light term goes to exactly 0 (not just
 * dim) in these fixtures, which reads as "nothing is rendering" rather than "the shadow is too
 * subtle to measure" -- see scene_mesh.yaml's header for the angle budget.
 *
 * scene_mesh.yaml's caster is a plain MeshRenderer, 1.5 x 1.5 m in footprint, so its shadow is a
 * narrow band and a left/right split of the visible floor strip cleanly separates lit from
 * shadowed. scene_particle.yaml's caster is a mesh-mode ParticleSystem instance (the
 * instanced-batch cull fix, a wholly separate code path in Engine::sync_particle_render_state_())
 * -- mesh-mode particles only scale uniformly (ParticleSystem::prepare_render()'s `size` applies
 * to all three axes), so this caster is a 10 x 10 x 10 cube, ten times wider than the mesh
 * caster, and its shadow is correspondingly wide: a center-vs-edge split at a depth close to the
 * caster, not a left/right split of the whole strip, is what isolates it.
 */

#include <coopa/testing/test.h>

#include <string>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("shadow_offscreen");

using namespace toy::test;

namespace {

/// Renders `scene` once, past the point its single scatter-mode particle (if any) has placed
/// its instance -- that needs a real (nonzero) dt, not FIXED_DT=0, the same reason particle
/// GPU tests elsewhere pin FIXED_DT to a real step instead of freezing time.
Frame render_scene(const std::string& scene) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config(scene, 640, 360, 320, 180));
    // Fog/sky dominate the beauty image at the distances this fixture's floor is seen from,
    // which would swamp the shadow's own contrast; "direct" reads the raw lit-vs-shadowed term,
    // same as local_light_shadows_test.cpp's precedent.
    engine.render_config().debug_view = "direct";
    // See this file's doc for why the test needs the flat reach to fall short here.
    engine.render_config().shadow_distance = 12.0f;
    tick_frames(engine, kNoiseCycle * 6);
    return engine.capture_image(true);
}

/// Compares a `shadow` region's mean brightness against a `lit` region's, both within the same
/// `[y0, y1]` depth band, and asserts the shadow side is measurably darker.
void expect_region_is_shadowed(const Frame& f, const std::string& label, float y0, float y1,
                               float shadow_x0, float shadow_x1, float lit_x0, float lit_x1) {
    const glm::vec3 lit = band_mean(f, y0, y1, lit_x0, lit_x1);
    const glm::vec3 shadowed = band_mean(f, y0, y1, shadow_x0, shadow_x1);
    const float darkening = (lit.r + lit.g + lit.b) - (shadowed.r + shadowed.g + shadowed.b);
    expect(darkening > 10.0f, label + ": the shadow side of the floor strip is measurably darker "
                                      "than the lit side (darkened by " +
                                   std::to_string(darkening) + " of 765)");
}

} // namespace

/**
 * @brief A plain MeshRenderer caster outside the camera frustum still shadows the visible
 *        floor -- the elevation-aware cascade reach (pipeline_shadows.cpp) must have gathered
 *        it into the directional cascade despite the caster being nowhere near the camera's
 *        own frustum.
 */
COOPA_TEST(mesh_caster_outside_frustum_still_shadows_the_visible_floor) {
    const Frame f = render_scene("tests/fixtures/scenes/shadow_offscreen/scene_mesh.yaml");
    // The floor fills roughly the lower half of the frame; at this depth the shadow's edge sits
    // at screen centre (world x = 0), so the left half of the visible strip is inside the
    // shadow and the right half is not -- see this file's doc for why.
    expect_region_is_shadowed(f, "mesh caster", 0.55f, 0.95f, 0.1f, 0.45f, 0.55f, 0.9f);
}

/**
 * @brief A mesh-mode ParticleSystem caster outside the camera frustum still shadows the
 *        visible floor -- Engine::sync_particle_render_state_() must widen its cull test along
 *        the sun for a casting batch instead of rejecting it outright on the camera frustum
 *        alone (engine_render_sync.cpp, render::sweep_bounds()).
 */
COOPA_TEST(particle_caster_outside_frustum_still_shadows_the_visible_floor) {
    const Frame f = render_scene("tests/fixtures/scenes/shadow_offscreen/scene_particle.yaml");
    // This caster's shadow is as wide as it is tall (see this file's doc), so it only clears
    // the visible strip's edges close to the camera, where perspective narrows the strip to
    // less than the shadow's own width. y in [0.70, 0.76] is that depth band; the shadow sits
    // centred in x there, with the strip's near edges still lit.
    expect_region_is_shadowed(f, "particle caster", 0.70f, 0.76f, 0.40f, 0.60f, 0.05f, 0.25f);
}
