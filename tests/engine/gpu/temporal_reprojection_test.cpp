/**
 * @file temporal_reprojection_test.cpp
 * @brief The temporal resolves follow moving objects: SSAO and SSR leave no trail where an object
 *        was, appear where it is, and release a frozen (still-camera) history when only the object
 *        moves -- the regressions behind AO and reflections sticking to moving objects.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <functional>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/scene/camera_controller.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("temporal_reprojection");

using namespace toy::test;

COOPA_TEST(ssao_follows_a_moving_object) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_shipped_config("assets/scenes/physics/cloth_demo/scene.yaml", 640, 360);
    // Eye adaptation would re-meter as the ball crosses the frame; the AO channel is what is
    // measured here, not exposure. SSAO itself ships on; assert that rather than assume it.
    config.render.auto_exposure_enabled = false;
    expect(config.render.ssao_enabled && config.render.ssao_temporal_enabled,
           "ssao tracks: the shipped config has SSAO and its temporal resolve on");
    config.render.ssao_enabled          = true;
    config.render.ssao_temporal_enabled = true;
    // The contact band's strength and width follow the gather radius, and the thresholds below
    // are set for a 2 m gather -- pinned, so retuning the shipped radius does not move them.
    config.render.ssao_radius           = 2.0f;
    toy::core::Engine engine(std::move(config));

    auto* ball  = engine.scene().find_object("ball");
    auto* cloth = engine.scene().find_object("cloth");
    auto* cc    = engine.scene().find_first_component<toy::scene::CameraController>();
    auto* cam   = coopa::gfx::engine::components::CameraComponent::main();
    expect(ball != nullptr && cloth != nullptr && cc != nullptr && cam != nullptr,
           "ssao tracks: the scene has the ball, the cloth, a CameraController and a main camera");
    if (!ball || !cloth || !cc || !cam) return;

    cloth->set_active(false);
    // A fixed orbit over the ball's whole path: start at x=0, destination at x=4.
    cc->tracker            = "";
    cc->target             = glm::vec3(2.0f, 0.0f, 0.5f);
    cc->target_offset      = glm::vec3(0.0f);
    cc->distance           = 9.0f;
    cc->pitch_deg          = 50.0f;
    cc->follow_smoothing   = 0.0f;
    cc->movement_smoothing = 0.0f;
    // Rest the unit sphere on the ground (its scene pose hovers 1.6 above it, where the
    // contact occlusion is too faint to measure).
    const glm::vec3 start(0.0f, 0.0f, 1.02f);
    const glm::vec3 dest (4.0f, 0.0f, 1.02f);
    ball->get_transform()->transform().set_position(start);

    engine.render_config().debug_view = "ssao";

    // The ground just in front of a ball resting at `at`, around its contact ring -- the
    // window below is wide enough to take in the whole band the sphere darkens there, which
    // from a 50-degree pitch is not hidden behind its silhouette.
    auto footprint = [](const glm::vec3& at) { return glm::vec3(at.x, at.y - 0.9f, 0.0f); };
    const glm::vec3 ref_point(-3.5f, -0.9f, 0.0f);

    // The occlusion at a world point: the mean of the darkest tenth of the red channel (the
    // "ssao" view is a greyscale readout) over a 65x65 window around the point projected into
    // the low-res capture with the engine's own NDC -> pixel convention (Engine::world_to_window,
    // with the capture's full extent as the rectangle). The darkest tenth, not the mean: the
    // contact band is a few pixels wide and its exact placement depends on the gather radius
    // and the blur, while the statistic only has to find it somewhere in the window. On open
    // ground the darkest tenth is simply the ground's own value.
    auto contact_ao_at = [&](const Frame& f, const glm::vec3& world, const char* what) -> double {
        const float aspect = static_cast<float>(f.width) / static_cast<float>(f.height);
        const glm::vec4 clip = cam->get_projection_matrix(aspect) * cam->get_view_matrix() * glm::vec4(world, 1.0f);
        expect(clip.w > 1e-6f, "ssao tracks: the sampled ground point is in front of the camera");
        if (clip.w <= 1e-6f) return 0.0;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const int cx = static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(f.width));
        const int cy = static_cast<int>((0.5f - ndc.y * 0.5f) * static_cast<float>(f.height));
        const int r = 32;
        const bool inside = cx >= r && cy >= r && cx < static_cast<int>(f.width) - r && cy < static_cast<int>(f.height) - r;
        expect(inside, std::string("ssao tracks: the ") + what + " sample window is on screen");
        if (!inside) return 0.0;
        std::vector<uint8_t> values;
        values.reserve(static_cast<size_t>((2 * r + 1) * (2 * r + 1)));
        for (int y = cy - r; y <= cy + r; ++y) {
            for (int x = cx - r; x <= cx + r; ++x) {
                values.push_back(f.pixels[(static_cast<size_t>(y) * f.width + static_cast<size_t>(x)) * f.channels]);
            }
        }
        std::sort(values.begin(), values.end());
        const size_t tenth = values.size() / 10;
        double sum = 0.0;
        for (size_t i = 0; i < tenth; ++i) sum += values[i];
        return sum / (static_cast<double>(tenth) * 255.0);
    };

    // Settle: camera pose, AO convergence, and the freeze (camera and scene both still).
    tick_frames(engine, 90);
    const Frame before = engine.capture_image(/*low_res=*/true);
    const double ref        = contact_ao_at(before, ref_point, "reference");
    const double start_dark = contact_ao_at(before, footprint(start), "start footprint");
    const double dest_open  = contact_ao_at(before, footprint(dest),  "destination footprint");
    // The contact occlusion must be a measurable signal, or nothing below means anything.
    const double margin = 0.05;
    expect(start_dark < ref - margin, "ssao tracks: the resting ball darkens the ground in front of it");
    expect(std::abs(dest_open - ref) < 0.03, "ssao tracks: the empty destination is as open as the reference patch");
    if (!(start_dark < ref - margin)) {
        std::cerr << "         ref " << ref << " start " << start_dark << " dest " << dest_open << "\n";
        dump_frame(before, "ssao_tracks_before");
    }
    // "Recovered" means the vacated ground has lost at least half of that contact darkness.
    // Not "equals the reference": ground the ball's silhouette just uncovered restarts its
    // accumulation from a single draw, and the darkest tenth of a window that is still
    // averaging its first frames reads a few levels low -- noise, not a trail. A stuck or
    // trailing history keeps the whole band, which this threshold catches.
    const double recovered = ref - 0.5 * (ref - start_dark);

    // Travel: 4 units in 60 ticks, the way cloth_scene_simulates_and_animates drives it.
    for (int i = 0; i < 60; ++i) {
        coopa::util::Transform& t = ball->get_transform()->transform();
        t.set_position(t.position() + glm::vec3(4.0f / 60.0f, 0.0f, 0.0f));
        engine.tick();
    }
    ball->get_transform()->transform().set_position(dest);
    // Two frames after the move stops: the 8-frame accumulation window has turned over.
    tick_frames(engine, 10);
    const Frame moved = engine.capture_image(true);
    const double ref_m   = contact_ao_at(moved, ref_point, "reference");
    const double start_m = contact_ao_at(moved, footprint(start), "start footprint");
    const double dest_m  = contact_ao_at(moved, footprint(dest),  "destination footprint");
    expect(start_m > recovered, "ssao tracks: no occlusion trails behind the ball where it used to rest");
    expect(dest_m < ref_m - margin, "ssao tracks: the ball darkens the ground at its destination");
    if (!(start_m > recovered) || !(dest_m < ref_m - margin)) {
        std::cerr << "         after move: ref " << ref_m << " start " << start_m << " dest " << dest_m << "\n";
        dump_frame(moved, "ssao_tracks_moved");
    }

    // Frozen case: camera and scene still long past the freeze threshold, then the ball jumps
    // back in a single tick. A freeze that only watched the camera held the stale image here.
    tick_frames(engine, 40);
    ball->get_transform()->transform().set_position(start);
    tick_frames(engine, 10);
    const Frame jumped = engine.capture_image(true);
    const double ref_j   = contact_ao_at(jumped, ref_point, "reference");
    const double start_j = contact_ao_at(jumped, footprint(start), "start footprint");
    const double dest_j  = contact_ao_at(jumped, footprint(dest),  "destination footprint");
    expect(dest_j > recovered, "ssao tracks: a frozen resolve releases when only the object moves");
    expect(start_j < ref_j - margin, "ssao tracks: the ball's occlusion reappears where it jumped to");
    if (!(dest_j > recovered) || !(start_j < ref_j - margin)) {
        std::cerr << "         after jump: ref " << ref_j << " start " << start_j << " dest " << dest_j << "\n";
        dump_frame(jumped, "ssao_tracks_jumped");
    }
}

/**
 * @brief Screen-space reflections must follow a moving object: its reflection appears where
 *        the object is and leaves no trail where it was -- including after the freeze.
 *
 * The SSR analogue of ssao_tracks_moving_object. The SSR temporal resolve reprojects through
 * the G-buffer velocity and the shared history count (TemporalHistoryPass) also reprojects by
 * velocity, so a moving object's reflection neither smears along its path nor sticks once the
 * camera is still; the hit colour comes from the previous frame's final image, reprojected by
 * the hit's own velocity.
 *
 * Scene: cloth_demo with the cloth switched off, the ground turned into a near-mirror, and a
 * low camera so the floor reflects faces of the ball the camera can see (screen-space rays
 * cannot hit the hidden underside). `debug_view: ssr_confidence` shows where reflection rays
 * found geometry: where the floor shows the ball's mirror image, and zero on open floor (rays
 * to the sky). Sampled at the mirror image of the ball's start and destination, and a far
 * reference patch.
 */
COOPA_TEST(ssr_follows_a_moving_object) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_shipped_config("assets/scenes/physics/cloth_demo/scene.yaml", 640, 360);
    config.render.auto_exposure_enabled = false;
    expect(config.render.ssr_enabled && config.render.ssr_temporal_enabled,
           "ssr tracks: the shipped config has SSR and its temporal resolve on");
    config.render.ssr_enabled          = true;
    config.render.ssr_temporal_enabled = true;
    toy::core::Engine engine(std::move(config));

    auto* ball   = engine.scene().find_object("ball");
    auto* cloth  = engine.scene().find_object("cloth");
    auto* ground = engine.scene().find_object("ground");
    auto* cc     = engine.scene().find_first_component<toy::scene::CameraController>();
    auto* cam    = coopa::gfx::engine::components::CameraComponent::main();
    expect(ball && cloth && ground && cc && cam,
           "ssr tracks: the scene has the ball, the cloth, the ground, a CameraController and a main camera");
    if (!ball || !cloth || !ground || !cc || !cam) return;
    auto* ground_mr = ground->get_component<coopa::gfx::engine::components::MeshRenderer>();
    expect(ground_mr != nullptr, "ssr tracks: the ground has a MeshRenderer");
    if (!ground_mr) return;

    cloth->set_active(false);
    ground_mr->material.roughness = 0.02f;
    ground_mr->material.metallic  = 1.0f;
    cc->tracker            = "";
    cc->target             = glm::vec3(2.0f, 0.0f, 0.5f);
    cc->target_offset      = glm::vec3(0.0f);
    cc->distance           = 9.0f;
    cc->pitch_deg          = 20.0f;   // low enough that the floor reflects the faces of the ball the camera sees
    cc->follow_smoothing   = 0.0f;
    cc->movement_smoothing = 0.0f;
    const glm::vec3 start(0.0f, 0.0f, 1.02f);
    const glm::vec3 dest (4.0f, 0.0f, 1.02f);
    ball->get_transform()->transform().set_position(start);

    engine.render_config().debug_view = "ssr_confidence";

    // Where the floor shows the ball's mirror image: the ball's centre reflected through the
    // floor (z = 0). The ball's controller holds its own hover height, so that height is read
    // back after the first tick rather than assumed from `start`.
    float ball_z = start.z;
    auto reflection = [&](const glm::vec3& at) { return glm::vec3(at.x, at.y, -ball_z); };

    // Mean of the brightest tenth of the red channel over a 49x49 window around the projected
    // world point -- finds the reflection's confident band wherever it lands in the window; on
    // open ground it is the (zero) miss confidence.
    auto confidence_at = [&](const Frame& f, const glm::vec3& world, const char* what) -> double {
        const float aspect = static_cast<float>(f.width) / static_cast<float>(f.height);
        const glm::vec4 clip = cam->get_projection_matrix(aspect) * cam->get_view_matrix() * glm::vec4(world, 1.0f);
        expect(clip.w > 1e-6f, "ssr tracks: the sampled ground point is in front of the camera");
        if (clip.w <= 1e-6f) return 0.0;
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const int cx = static_cast<int>((ndc.x * 0.5f + 0.5f) * static_cast<float>(f.width));
        const int cy = static_cast<int>((0.5f - ndc.y * 0.5f) * static_cast<float>(f.height));
        const int r = 24;
        const bool inside = cx >= r && cy >= r && cx < static_cast<int>(f.width) - r && cy < static_cast<int>(f.height) - r;
        expect(inside, std::string("ssr tracks: the ") + what + " sample window is on screen");
        if (!inside) return 0.0;
        std::vector<uint8_t> values;
        values.reserve(static_cast<size_t>((2 * r + 1) * (2 * r + 1)));
        for (int y = cy - r; y <= cy + r; ++y) {
            for (int x = cx - r; x <= cx + r; ++x) {
                values.push_back(f.pixels[(static_cast<size_t>(y) * f.width + static_cast<size_t>(x)) * f.channels]);
            }
        }
        std::sort(values.begin(), values.end(), std::greater<uint8_t>());
        const size_t tenth = values.size() / 10;
        double sum = 0.0;
        for (size_t i = 0; i < tenth; ++i) sum += values[i];
        return sum / (static_cast<double>(tenth) * 255.0);
    };

    tick_frames(engine, 90);
    ball_z = ball->get_transform()->transform().position().z;
    // Open floor well away from both positions: its rays all miss into the sky.
    const glm::vec3 ref_point(-3.5f, 0.0f, -ball_z);
    const Frame before = engine.capture_image(/*low_res=*/true);
    const double ref        = confidence_at(before, ref_point, "reference");
    const double start_lit  = confidence_at(before, reflection(start), "start reflection");
    const double dest_empty = confidence_at(before, reflection(dest),  "destination reflection");
    const double margin = 0.15;
    expect(start_lit > ref + margin, "ssr tracks: the mirror floor reflects the resting ball");
    expect(std::abs(dest_empty - ref) < 0.05, "ssr tracks: the empty destination reflects nothing, like the reference");
    if (!(start_lit > ref + margin) || !(std::abs(dest_empty - ref) < 0.05)) {
        std::cerr << "         ref " << ref << " start " << start_lit << " dest " << dest_empty << "\n";
        dump_frame(before, "ssr_tracks_before");
    }
    // A trail keeps most of the reflection's confidence where the ball was; the resolve must
    // have dropped at least half of it.
    const double recovered = ref + 0.5 * (start_lit - ref);

    for (int i = 0; i < 60; ++i) {
        coopa::util::Transform& t = ball->get_transform()->transform();
        t.set_position(t.position() + glm::vec3(4.0f / 60.0f, 0.0f, 0.0f));
        engine.tick();
    }
    ball->get_transform()->transform().set_position(dest);
    tick_frames(engine, 2);
    const Frame moved = engine.capture_image(true);
    const double ref_m   = confidence_at(moved, ref_point, "reference");
    const double start_m = confidence_at(moved, reflection(start), "start reflection");
    const double dest_m  = confidence_at(moved, reflection(dest),  "destination reflection");
    expect(start_m < recovered, "ssr tracks: no reflection trails behind the ball where it used to rest");
    expect(dest_m > ref_m + margin, "ssr tracks: the ball's reflection appears at its destination");
    if (!(start_m < recovered) || !(dest_m > ref_m + margin)) {
        std::cerr << "         after move: ref " << ref_m << " start " << start_m << " dest " << dest_m << "\n";
        dump_frame(moved, "ssr_tracks_moved");
    }

    // Frozen case: everything still past the freeze threshold, then the ball jumps back.
    tick_frames(engine, 40);
    ball->get_transform()->transform().set_position(start);
    tick_frames(engine, 10);
    const Frame jumped = engine.capture_image(true);
    const double ref_j   = confidence_at(jumped, ref_point, "reference");
    const double start_j = confidence_at(jumped, reflection(start), "start reflection");
    const double dest_j  = confidence_at(jumped, reflection(dest),  "destination reflection");
    expect(dest_j < recovered, "ssr tracks: a frozen resolve releases when only the object moves");
    expect(start_j > ref_j + margin, "ssr tracks: the reflection reappears where the ball jumped to");
    if (!(dest_j < recovered) || !(start_j > ref_j + margin)) {
        std::cerr << "         after jump: ref " << ref_j << " start " << start_j << " dest " << dest_j << "\n";
        dump_frame(jumped, "ssr_tracks_jumped");
    }
}
