/**
 * @file debug_overlay_test.cpp
 * @brief The debug overlay's CPU half (toyengine/debug/debug_overlay.h): frame-time ring
 *        statistics across a wrap, mode names and cycling, and watch()/text() lines (ignored while
 *        off, replaced in place). Drawing it is debug_overlay_render's.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <string>

#include <glm/glm.hpp>
#include <toyengine/debug/debug_overlay.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("debug_overlay");

/**
 * @brief The debug overlay's CPU half (toyengine/debug/debug_overlay.h): the frame-time ring's
 *        statistics across a wrap, the mode names, and the toy::debug::watch()/text() lines --
 *        ignored while the overlay is off, replaced in place on a repeat, shown in the Game block.
 */
COOPA_TEST(frame_ring_statistics_and_watch_lines) {
    using namespace toy::debug;
    FrameTimeRing ring;
    expect(ring.empty() && ring.average() == 0.0f && ring.low_1pct_fps() == 0.0f, "an empty ring reads 0");
    ring.push(10.0f);
    ring.push(20.0f);
    ring.push(30.0f);
    expect_near(ring.average(), 20.0f, 1e-4f, "ring average");
    expect_near(ring.min(), 10.0f, 1e-4f, "ring min");
    expect_near(ring.max(), 30.0f, 1e-4f, "ring max");
    expect_near(ring.low_1pct_fps(), 1000.0f / 30.0f, 1e-3f, "the 1% low of a short ring is its slowest frame");

    ring.clear();
    for (int i = 0; i < 300; ++i) ring.push(static_cast<float>(i));
    expect(ring.size() == FrameTimeRing::kCapacity, "the ring keeps only the last 240 frames");
    expect_near(ring.at(0), 60.0f, 1e-4f, "oldest sample after a wrap");
    expect_near(ring.at(FrameTimeRing::kCapacity - 1), 299.0f, 1e-4f, "newest sample after a wrap");

    ring.clear();
    for (int i = 0; i < 238; ++i) ring.push(10.0f);
    ring.push(50.0f);
    ring.push(30.0f);
    expect_near(ring.low_1pct_fps(), 25.0f, 1e-3f, "the 1% low averages the slowest 1% (2 of 240) frames");

    expect(parse_overlay_mode("full") == OverlayMode::Full && parse_overlay_mode("fps") == OverlayMode::Fps &&
           parse_overlay_mode("off") == OverlayMode::Off && !parse_overlay_mode("loud"), "overlay mode names parse");
    expect(next_overlay_mode(OverlayMode::Off) == OverlayMode::Fps && next_overlay_mode(OverlayMode::Fps) == OverlayMode::Full &&
           next_overlay_mode(OverlayMode::Full) == OverlayMode::Off, "F3 cycles off -> fps -> full -> off");

    GameLines& lines = GameLines::instance();
    {
        DebugOverlay off;
        watch("ignored", 1);
        text("ignored");
        expect(lines.empty(), "watch()/text() publish nothing while the overlay is off");
        expect(off.compose_lines().empty(), "an off overlay has no lines");
    }
    {
        DebugOverlay overlay(OverlayMode::Fps);
        overlay.record_frame(500.0f);   // the hitch that showed it: dropped
        overlay.record_frame(20.0f);
        expect(overlay.ring().size() == 1, "the first frame after showing the overlay is not recorded");
        watch("speed", 1.5f);
        watch("grounded", true);
        watch("speed", 2);
        watch("pos", glm::vec3(1.0f, 2.0f, 3.0f));
        text("state: %s %d", "run", 3);
        expect(lines.watches().size() == 3 && lines.watches()[0].name == "speed" && lines.watches()[0].value == "2",
               "a repeated watch() replaces its value in place");
        expect(lines.watches()[1].value == "true" && lines.watches()[2].value == "(1.000, 2.000, 3.000)",
               "watch() formats bools and vectors");
        const auto composed = overlay.compose_lines();
        auto has = [&](const std::string& label, const std::string& value) {
            return std::any_of(composed.begin(), composed.end(), [&](const DebugOverlay::Line& l) {
                return l.label == label && (value.empty() || l.value == value);
            });
        };
        expect(has("FPS", "50") && has("Game", "") && has("speed", "2") && has("state: run 3", ""),
               "fps mode shows the frame rate and the Game block");
        expect(!has("Render", ""), "fps mode leaves out the full-mode blocks");

        overlay.set_mode(OverlayMode::Full);
        overlay.stats().has_physics = true;
        overlay.stats().bodies = 12;
        overlay.stats().bodies_awake = 3;
        const auto full = overlay.compose_lines();
        auto has_full = [&](const std::string& label, const std::string& value) {
            return std::any_of(full.begin(), full.end(), [&](const DebugOverlay::Line& l) {
                return l.label == label && (value.empty() || l.value == value);
            });
        };
        expect(has_full("Render", "") && has_full("Scene", "") && has_full("bodies awake / all", "3 / 12") &&
               !has_full("Navigation", ""), "full mode adds the render, scene and physics blocks (nav only with a NavSystem)");

        lines.clear();
        expect(lines.empty(), "the per-frame clear empties the Game block");
        overlay.set_mode(OverlayMode::Off);
        watch("after", 1);
        expect(lines.empty() && !lines.accepting(), "turning the overlay off stops accepting lines");
    }
}
