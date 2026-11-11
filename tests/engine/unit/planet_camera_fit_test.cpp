/**
 * @file planet_camera_fit_test.cpp
 * @brief PlanetCameraFit derives an orbit camera's zoom range, zoom rate and clip planes from the
 *        scene's planet, so none of them is a magic multiple of a radius that can change.
 *
 * No device and no window: CameraController takes its input through plain fields, and the fit is
 * arithmetic over the terrain's params and its baked height.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

#include <glm/glm.hpp>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/world/planet_camera_fit.h>
#include <toyengine/world/terrain_component.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("planet_camera_fit");

using coopa::gfx::engine::components::CameraComponent;
using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
using toy::scene::CameraControlMode;
using toy::scene::CameraController;
using toy::world::PlanetCameraFit;
using toy::world::TerrainComponent;

namespace {

/**
 * @brief A planet (face_tiles 64, tile_size 1 -> radius ~40.74) with one tile raised to
 *        `peak_steps`, and a camera object carrying Camera + CameraController + PlanetCameraFit.
 */
struct Fixture {
    Scene scene{"planet_camera_fit_test"};
    TerrainComponent* terrain = nullptr;
    CameraComponent*  camera  = nullptr;
    CameraController* orbit   = nullptr;
    PlanetCameraFit*  fit     = nullptr;

    explicit Fixture(std::int32_t peak_steps, bool with_planet = true, float fov = 50.0f) {
        if (with_planet) {
            auto planet = std::make_unique<SceneObject>("planet");
            planet->add_component<TransformComponent>();
            terrain = planet->add_component<TerrainComponent>();
            terrain->params.planet      = true;
            terrain->params.face_tiles  = 64;
            terrain->params.tile_size   = 1.0f;
            terrain->params.chunk_size  = 8;
            terrain->params.height_step = 1.0f;

            toy::world::TerraGeometry g;
            g.tiles_x     = 6 * terrain->params.face_tiles;   // six faces side by side
            g.tiles_y     = terrain->params.face_tiles;
            g.tile_size   = terrain->params.tile_size;
            g.height_step = terrain->params.height_step;
            g.chunk_tiles = terrain->params.chunk_size;
            terrain->field.reset(g);
            terrain->field.data()->at(3, 3).steps = peak_steps;
            terrain->field.mark_all_dirty();
            scene.add_root_object(std::move(planet));
        }

        auto cam = std::make_unique<SceneObject>("camera");
        auto* tc = cam->add_component<TransformComponent>();
        tc->transform().set_position(glm::vec3(0.0f, -200.0f, 0.0f));
        camera = cam->add_component<CameraComponent>();
        camera->fov        = fov;
        camera->clip_start = 1.0f;
        camera->clip_end   = 5000.0f;
        orbit = cam->add_component<CameraController>();
        orbit->mode         = CameraControlMode::Orbit;
        orbit->target       = glm::vec3(0.0f);
        orbit->yaw_deg      = 0.0f;
        orbit->pitch_deg    = 20.0f;
        orbit->min_distance = 1.0f;
        orbit->max_distance = 100.0f;
        orbit->zoom_speed   = 1.0f;
        fit = cam->add_component<PlanetCameraFit>();
        scene.add_root_object(std::move(cam));

        toy::world::install_planet_camera_fit_system(scene);
        scene.start();
    }

    float radius() const { return terrain->params.layout().radius(); }
    /** @brief The distance a sphere of `radius()` exactly fills the frame at, times the margin. */
    float fitted(float margin = 1.1f) const {
        return radius() / std::sin(glm::radians(50.0f * 0.5f)) * margin;
    }
};

} // namespace

COOPA_TEST(it_frames_the_planet_at_max_distance) {
    Fixture f(40);
    f.scene.update(1.0f / 60.0f);

    expect_near(f.orbit->max_distance, f.fitted(), 1e-2f,
                "full zoom-out is the distance the globe exactly fills the frame at, plus the margin");
    expect(f.orbit->max_distance > f.radius(),
           "which is outside the planet, not inside it");
    expect_near(f.orbit->distance, f.orbit->max_distance, 1e-2f,
                "and play opens there, with the globe held whole");
}

COOPA_TEST(min_distance_clears_the_measured_peak) {
    // The bug this exists to stop: planet_demo authored `min_distance: R + 4` on a globe whose
    // mountains stand a third of its radius tall, so full zoom-in was inside the terrain.
    Fixture f(30);
    f.scene.update(1.0f / 60.0f);

    expect_near(f.orbit->min_distance, f.radius() + 30.0f + 4.0f, 1e-2f,
                "closest zoom clears the measured peak by min_clearance");
    expect(f.orbit->min_distance > f.radius() + 30.0f,
           "so the camera is never inside a mountain");
}

COOPA_TEST(it_reads_the_measured_peak_not_height_scale) {
    // params.height_scale is a nominal amplitude, and a scene authoring `height_scale:` on its
    // MapGenerator leaves it at the default entirely -- it only reaches params on an apply.
    Fixture f(30);
    f.terrain->params.height_scale = 999.0f;   // a lie, as far as the baked world is concerned
    f.scene.update(1.0f / 60.0f);

    expect_near(f.orbit->min_distance, f.radius() + 30.0f + 4.0f, 1e-2f,
                "the clearance follows the baked height, not the nominal one");
}

COOPA_TEST(the_zoom_coupled_pitch_curve_is_unchanged) {
    // The headline guarantee. CameraController normalises the zoom as
    // (distance - min) / (max - min) to drive pitch and look tilt, so deriving a SUBSET of the
    // three would move where an authored pitch_at_*_distance_deg is reached. Sampling the curve
    // at the same three fractions of the range, with and without the fit, must agree.
    const auto pitch_at_fractions = [](Fixture& f) {
        std::array<float, 3> out{};
        const float lo = f.orbit->min_distance, hi = f.orbit->max_distance;
        int i = 0;
        for (float t : {0.0f, 0.5f, 1.0f}) {
            f.orbit->distance = lo + (hi - lo) * t;
            f.orbit->snap_to_targets();
            f.orbit->update(1.0f / 60.0f);
            out[i++] = f.orbit->pitch_deg;
        }
        return out;
    };

    // No planet: the fit no-ops, so this is the authored rig's own curve.
    Fixture authored_rig(0, /*with_planet=*/false);
    authored_rig.orbit->pitch_at_min_distance_deg = 8.0f;
    authored_rig.orbit->pitch_at_max_distance_deg = 70.0f;
    authored_rig.scene.update(1.0f / 60.0f);
    const std::array<float, 3> authored = pitch_at_fractions(authored_rig);

    // With a planet: the fit rewrites all three distances, over a range an order of magnitude
    // wider than the authored 1..100.
    Fixture fitted(40);
    fitted.orbit->pitch_at_min_distance_deg = 8.0f;
    fitted.orbit->pitch_at_max_distance_deg = 70.0f;
    fitted.scene.update(1.0f / 60.0f);
    const std::array<float, 3> derived = pitch_at_fractions(fitted);

    // Both endpoints move, and the span with them, so the agreement below is a real invariance
    // and not two identical ranges being compared.
    expect(std::abs(fitted.orbit->min_distance - authored_rig.orbit->min_distance) > 10.0f &&
               std::abs(fitted.orbit->max_distance - authored_rig.orbit->max_distance) > 1.0f,
           "the derived range really is a different range");
    for (int i = 0; i < 3; ++i) {
        expect_near(derived[i], authored[i], 1e-2f,
                    "the pitch at each fraction of the zoom range is where the scene put it");
    }
    expect_near(derived[0], 8.0f, 1e-2f, "the zoomed-in end still reaches its authored pitch");
    expect_near(derived[2], 70.0f, 1e-2f, "as does the zoomed-out end");
}

COOPA_TEST(the_first_apply_does_not_smooth_chase) {
    // start() has already seeded the applied pose from the AUTHORED distance, and
    // smoothed_distance_ is private -- so without snap_to_targets() the camera would visibly
    // slide from the authored 100 to the derived fit over the first second of play.
    Fixture f(40);
    f.orbit->movement_smoothing = 0.25f;
    f.scene.update(1.0f / 60.0f);

    expect_near(f.orbit->orbit_distance(), f.orbit->max_distance, 1e-2f,
                "the APPLIED orbit radius is already the derived one on frame one");
    expect(f.orbit->orbit_distance() > 100.0f,
           "not still at, or easing up from, the authored 100");
}

COOPA_TEST(it_derives_the_clip_planes) {
    Fixture f(40);
    f.fit->near_clip_ratio = 2e-4f;
    f.scene.update(1.0f / 60.0f);

    const float expected_far = (f.orbit->max_distance + f.radius()) * 2.0f;
    expect_near(f.camera->clip_end, expected_far, 1e-1f,
                "the far plane clears the whole globe from the far end of the zoom, with headroom");
    expect_near(f.camera->clip_start, expected_far * 2e-4f, 1e-4f,
                "and the near plane is a ratio of it, which is what depth precision depends on");
}

COOPA_TEST(far_clip_scale_zero_leaves_the_camera_alone) {
    Fixture f(40);
    f.fit->far_clip_scale = 0.0f;
    f.scene.update(1.0f / 60.0f);

    expect_near(f.camera->clip_end, 5000.0f, 1e-3f, "the authored far plane is untouched");
    expect_near(f.camera->clip_start, 1.0f, 1e-3f, "and so is the near one");
}

COOPA_TEST(a_scene_with_no_planet_is_a_no_op) {
    Fixture f(0, /*with_planet=*/false);
    const float min_d = f.orbit->min_distance, max_d = f.orbit->max_distance;
    const float zoom = f.orbit->zoom_speed, far_c = f.camera->clip_end;
    f.scene.update(1.0f / 60.0f);

    expect_near(f.orbit->min_distance, min_d, 1e-6f, "no planet, no framing: min_distance as authored");
    expect_near(f.orbit->max_distance, max_d, 1e-6f, "max_distance as authored");
    expect_near(f.orbit->zoom_speed, zoom, 1e-6f, "zoom_speed as authored");
    expect_near(f.camera->clip_end, far_c, 1e-6f, "and the clip planes as authored");
}

COOPA_TEST(a_fly_camera_is_untouched) {
    Fixture f(40);
    f.orbit->mode = CameraControlMode::Fly;
    const float max_d = f.orbit->max_distance;
    f.scene.update(1.0f / 60.0f);

    expect_near(f.orbit->max_distance, max_d, 1e-6f,
                "a zoom range means nothing to a fly camera, so it is left alone");
}

COOPA_TEST(a_later_bake_corrects_the_framing_without_jumping) {
    // The async case: with no .terra the world bakes on a job, so the first apply sees peak 0 and
    // a frame later the real height lands. The revision gate must pick that up -- and must NOT
    // snap, because easing into a corrected framing reads better than a jump mid-play.
    Fixture f(0);
    f.scene.update(1.0f / 60.0f);
    const float flat_min = f.orbit->min_distance;

    f.terrain->field.data()->at(5, 5).steps = 25;
    f.terrain->field.mark_all_dirty();
    f.scene.update(1.0f / 60.0f);

    expect(f.orbit->min_distance > flat_min + 20.0f,
           "the clearance grows with the mountains the bake just produced");
    expect_near(f.orbit->min_distance, f.radius() + 25.0f + 4.0f, 1e-2f,
                "to exactly the new peak plus the clearance");
}

COOPA_TEST(an_edited_knob_re_applies) {
    Fixture f(40);
    f.scene.update(1.0f / 60.0f);
    const float before = f.orbit->max_distance;

    f.fit->max_distance_scale = 2.0f;
    f.scene.update(1.0f / 60.0f);

    expect_near(f.orbit->max_distance, before * 2.0f, 1e-2f,
                "the knobs are in the settings hash, so an inspector drag reaches the camera");
}

COOPA_TEST(an_impossible_range_leaves_the_camera_as_authored) {
    // max <= min would collapse the normalised zoom the pitch and tilt curves read, locking the
    // camera at one elevation with nothing on screen to say why.
    Fixture f(40);
    f.fit->max_distance_scale = 0.01f;
    const float min_d = f.orbit->min_distance, max_d = f.orbit->max_distance;
    f.scene.update(1.0f / 60.0f);

    expect_near(f.orbit->min_distance, min_d, 1e-6f, "nothing is written when the range inverts");
    expect_near(f.orbit->max_distance, max_d, 1e-6f, "so the authored rig still works");
}
