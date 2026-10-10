/**
 * @file debug_overlay_render_test.cpp
 * @brief The debug overlay drawn: fps mode puts a panel and text in a corner, full mode runs a
 *        live profile, and off is free -- no layer, no profiler, byte-identical to never showing it.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/debug/debug_overlay.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("debug_overlay_render");

using namespace toy::test;

/**
 * @brief The debug overlay end to end: fps mode draws its panel and text over the frame as an
 *        engine overlay layer, full mode runs a live profile and shows the game's watch() lines,
 *        and turning it off costs nothing -- the layer and the profiler are gone, and the frame
 *        is byte-identical to the same frame of an engine that never showed it.
 *
 * kitchen_sink's image moves a little from frame to frame even at FIXED_DT 0 (frame-indexed
 * noise), so "off" is compared against a second Engine at the same frame count rather than
 * against this one's earlier frames. Within one frame index, everything that differs is the
 * overlay's.
 */
COOPA_TEST(fps_and_full_modes_draw_and_off_is_free) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    const toy::core::AppConfig config = make_test_config("tests/fixtures/scenes/kitchen_sink/scene.yaml", 640, 360, 160, 90);
    // Frames: 3 off, 3 fps, 20 full, 3 off.
    const int kTotalFrames = 29;

    // The reference: an Engine that never shows the overlay, at each frame count compared.
    Frame ref_fps;
    Frame ref_off;
    {
        toy::core::Engine engine(config);
        tick_frames(engine, 6);
        ref_fps = engine.capture_image(/*low_res=*/false);
        tick_frames(engine, kTotalFrames - 6);
        ref_off = engine.capture_image(false);
        expect(engine.overlay_layers().empty() && !engine.frame_profile(), "debug overlay: off by default (no layer, no profiler)");
    }

    toy::core::Engine engine(config);
    toy::core::FrameHooks hooks;
    hooks.pre_scene_update = [](float) { toy::debug::watch("probe", 42); };
    engine.set_frame_hooks(std::move(hooks));
    tick_frames(engine, 3);

    engine.debug_overlay().set_mode(toy::debug::OverlayMode::Fps);
    tick_frames(engine, 3);
    const Frame fps = engine.capture_image(false);
    expect(engine.overlay_layers().size() == 1, "debug overlay: fps mode registers one overlay layer");
    expect(same_extent(fps, ref_fps), "debug overlay: captures share one extent");
    if (!same_extent(fps, ref_fps)) return;
    const long long fps_px = count_diff(fps, ref_fps);
    bool ok = expect_at_least(fps_px, 500, "debug overlay: fps mode draws its panel over the frame");
    // Text: the panel darkens everything under it, so a pixel BRIGHTER than the same pixel of
    // the reference can only be a glyph (or one of the sparkline's few bars).
    long long text_px = 0;
    for (size_t i = 0; i < static_cast<size_t>(fps.width) * fps.height; ++i) {
        for (int ch = 0; ch < 3; ++ch) {
            if (int(fps.pixels[i * fps.channels + ch]) > int(ref_fps.pixels[i * ref_fps.channels + ch]) + 30) { ++text_px; break; }
        }
    }
    ok &= expect_at_least(text_px, 100, "debug overlay: fps mode draws text");
    const long long pixels = static_cast<long long>(fps.width) * fps.height;
    ok &= expect(fps_px < pixels / 3, "debug overlay: the panel covers a corner, not the frame");

    engine.debug_overlay().set_mode(toy::debug::OverlayMode::Full);
    tick_frames(engine, 20);
    const Frame full = engine.capture_image(false);
    expect(engine.frame_profile() && engine.frame_profile()->live(), "debug overlay: full mode runs a live profile");
    expect(engine.frame_profile() && engine.frame_profile()->latest(), "debug overlay: the live profile completes frames");
    const auto lines = engine.debug_overlay().compose_lines();
    expect(std::any_of(lines.begin(), lines.end(), [](const auto& l) { return l.label == "Render"; }),
           "debug overlay: full mode has its Render block");
    ok &= expect(count_diff(full, ref_off) > fps_px, "debug overlay: full mode shows more than fps mode");

    engine.debug_overlay().set_mode(toy::debug::OverlayMode::Off);
    tick_frames(engine, kTotalFrames - 26);
    const Frame off = engine.capture_image(false);
    expect(engine.overlay_layers().empty() && !engine.frame_profile(), "debug overlay: off removes the layer and the profiler");
    const long long off_px = count_diff(off, ref_off);
    ok &= expect(off_px == 0, "debug overlay: off is byte-identical to never having shown it (" + std::to_string(off_px) + " px differ)");

    if (!ok) {
        dump_frame(ref_fps, "debug_overlay_ref_fps");
        dump_frame(fps, "debug_overlay_fps");
        dump_frame(full, "debug_overlay_full");
        dump_frame(ref_off, "debug_overlay_ref_off");
        dump_frame(off, "debug_overlay_off");
    }
}
