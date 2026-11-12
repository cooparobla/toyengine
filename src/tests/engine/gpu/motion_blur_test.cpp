/**
 * @file motion_blur_test.cpp
 * @brief Motion blur (toyengine/render/passes/motion_blur_pass.h): off records nothing and is
 *        byte-identical after a round trip, and a block sliding along X smears beside itself only.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/kinematic_mover.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("motion_blur");

using namespace toy::test;

/**
 * @brief Motion blur (toyengine/render/passes/motion_blur_pass.h), two claims on one Engine over
 *        assets/scenes/rendering/motion_blur_demo with its scripted movers removed, so every
 *        motion below is driven by hand under FIXED_DT=0:
 *   - off is free: with `motion_blur` false nothing is recorded, and toggling it on and back off
 *     leaves the capture byte-identical (SSR off, and the two captures 256 frames apart, the
 *     period of the frame-indexed shadow noise);
 *   - a block sliding along X smears beside itself and nowhere above or below.
 * Every capture comes kNoiseCycle ticks after a reset, so on/off pairs see identical poses.
 */
COOPA_TEST(off_is_free_and_a_moving_block_smears_sideways) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/rendering/motion_blur_demo/scene.yaml", 640, 360, 320, 180);
    config.render.ssr_enabled = false;   // its frame-indexed trace dither would differ between captures
    toy::core::Engine engine(std::move(config));
    auto& rc = engine.render_config();
    auto& scene = engine.scene();
    auto* mover   = scene.find_object("mover");
    auto* spinner = scene.find_object("spinner");
    auto* camera  = scene.find_object("camera");
    expect(mover && spinner && camera, "motion blur: the test scene has its mover, spinner and camera");
    if (!mover || !spinner || !camera) return;
    mover->remove_component<toy::scene::KinematicMover>();
    spinner->set_active(false);
    if (auto* orbit = camera->get_component<toy::scene::CameraController>()) orbit->auto_rotate_deg_per_sec = 0.0f;

    rc.outline_enabled = rc.palette_enabled = rc.dither_enabled = false;   // the plain HDR image
    rc.motion_blur_intensity  = 1.0f;     // a full-frame shutter: the strongest signal
    const glm::vec3 start(-2.5f, 3.0f, 1.2f);
    auto run = [&](bool blur, float step_m) {
        rc.motion_blur = blur;
        for (int i = 0; i < kNoiseCycle; ++i) {
            mover->get_transform()->transform().set_position(start + glm::vec3(step_m * float(i), 0.0f, 0.0f));
            engine.tick();
        }
        return engine.capture_image(true);
    };

    // --- Off is free ---
    rc.motion_blur = false;
    tick_frames(engine, kNoiseCycle);
    run(false, 0.0f);   // settle after moving the block to its start
    const uint64_t recorded0 = engine.pipeline().motion_blur_frames();
    const Frame static_off = run(false, 0.0f);
    expect(engine.pipeline().motion_blur_frames() == recorded0, "motion blur off: no pass is recorded");
    run(true, 0.0f);
    expect(engine.pipeline().motion_blur_frames() >= recorded0 + kNoiseCycle, "motion blur on: the pass records every frame");
    rc.motion_blur = false;
    const uint64_t recorded1 = engine.pipeline().motion_blur_frames();
    // The shadow PCF kernel rotates with frame_index_ & 0xFF, so the comparison capture lands
    // exactly 256 frames after the first one.
    tick_frames(engine, 256 - 2 * kNoiseCycle);
    const Frame static_off_again = run(false, 0.0f);
    expect(engine.pipeline().motion_blur_frames() == recorded1, "motion blur switched off at runtime: no pass is recorded");
    const long long off_diff = count_diff(static_off_again, static_off);
    expect(off_diff == 0, "motion blur switched back off: the capture is byte-identical to before");
    if (off_diff != 0) {
        std::cerr << "         off/off diff " << off_diff << " px\n";
        dump_frame(static_off, "motion_blur_off_a");
        dump_frame(static_off_again, "motion_blur_off_b");
    }

    // --- A block sliding along +X: blur beside it, none above or below ---
    const float step = 0.8f;   // m per frame: ~10 px at this distance and resolution
    const Frame moving_off = run(false, step);
    const Frame moving_on  = run(true, step);
    // Where the block is: the same frame without it.
    mover->set_active(false);
    const Frame no_block = run(false, 0.0f);
    mover->set_active(true);
    uint32_t bx0 = moving_off.width, by0 = moving_off.height, bx1 = 0, by1 = 0;
    for (uint32_t y = 0; y < moving_off.height; ++y)
        for (uint32_t x = 0; x < moving_off.width; ++x) {
            const size_t i = (static_cast<size_t>(y) * moving_off.width + x) * moving_off.channels;
            bool d = false;
            for (int ch = 0; ch < 3; ++ch) d |= std::abs(int(moving_off.pixels[i + ch]) - int(no_block.pixels[i + ch])) > 24;
            // The block's own pixels only, not its shadow on the floor below it.
            if (d && moving_off.pixels[i] > 150) { bx0 = std::min(bx0, x); by0 = std::min(by0, y); bx1 = std::max(bx1, x); by1 = std::max(by1, y); }
        }
    expect(bx1 > bx0 + 4 && by1 > by0 + 4, "motion blur: the block is on screen");
    if (bx1 <= bx0 + 4 || by1 <= by0 + 4) { dump_frame(moving_off, "motion_blur_moving_off"); return; }
    long long beside = 0, above_below = 0;
    const int margin = 12;
    for (uint32_t y = 0; y < moving_off.height; ++y)
        for (uint32_t x = 0; x < moving_off.width; ++x) {
            const size_t i = (static_cast<size_t>(y) * moving_off.width + x) * moving_off.channels;
            bool d = false;
            for (int ch = 0; ch < 3; ++ch) d |= std::abs(int(moving_on.pixels[i + ch]) - int(moving_off.pixels[i + ch])) > 8;
            if (!d) continue;
            const bool in_rows = y >= by0 && y <= by1;
            const bool in_cols = x >= bx0 && x <= bx1;
            const bool near_rows = int(y) >= int(by0) - margin && int(y) <= int(by1) + margin;
            const bool near_cols = int(x) >= int(bx0) - margin && int(x) <= int(bx1) + margin;
            if (in_rows && near_cols && !in_cols) ++beside;
            if (in_cols && near_rows && !in_rows) ++above_below;
        }
    const long long block_rows = static_cast<long long>(by1 - by0 + 1);
    expect_at_least(beside, block_rows, "motion blur: a block moving along X smears beside itself");
    expect(above_below <= block_rows / 4, "motion blur: ...and not above or below it");
    if (beside < block_rows || above_below > block_rows / 4) {
        std::cerr << "         block [" << bx0 << "," << by0 << "]-[" << bx1 << "," << by1 << "]: " << beside
                  << " px changed beside it, " << above_below << " above/below\n";
        dump_frame(moving_off, "motion_blur_moving_off");
        dump_frame(moving_on, "motion_blur_moving_on");
    }

    mover->set_active(true);
}
