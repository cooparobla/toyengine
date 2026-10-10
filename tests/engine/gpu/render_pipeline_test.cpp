/**
 * @file render_pipeline_test.cpp
 * @brief The render pipeline end to end on assets/scenes/demos/pixel_demo: the frame has the
 *        configured size, is reproducible under FIXED_DT=0 (which every diff threshold in the GPU
 *        suites leans on), every live toggle reaches the image, every STARTUP-FIXED toggle's "off"
 *        construction branch still renders, and the scene packaged as .caml renders byte-identically.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <iterator>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <toyengine/core/caml_codec.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
#include "engine/support/scratch_files.h"

COOPA_TEST_SUITE("render_pipeline");

using namespace toy::test;

namespace {

/// The eight pico-8 entries assets/scenes/pixel_demo actually resolves to.
const uint8_t kPico8Subset[8][3] = {
    {0, 0, 0}, {29, 43, 83}, {126, 37, 83}, {0, 135, 81},
    {171, 82, 54}, {95, 87, 79}, {194, 195, 199}, {255, 241, 232},
};

} // namespace

/**
 * @brief One Engine, four claims: the render path produces exactly the configured
 * buffer, every pixel of it is a palette entry, the frames are reproducible, and each live
 * toggle measurably changes the image.
 *
 * The palette assertion alone validates the render path, the outline/dither/quantize post pass
 * and the UNORM/gamma choice together -- see gfxcoopa's pixel_stylize_pass.h file doc. What is
 * new here is the SECOND assertion: two captures a noise cycle apart, with nothing
 * changed, must be byte-identical. That is not a property of the renderer being tested for its
 * own sake -- it is what earns the right to compare the toggle frames below with exact counts
 * instead of a guessed tolerance. If it ever fails, every diff threshold in this group is
 * measuring noise and should be disbelieved before the toggles are.
 *
 * The three toggles are then flipped on the LIVE pipeline (palette, then SDF, then outline),
 * which is the whole reason this is one test and not four Engines: each is re-read from the
 * config every frame (see PixelRenderPipeline::render_config_mut()), so a flip plus a
 * noise cycle of ticks costs milliseconds against the ~second a fresh device, pipeline set and scene load costs.
 * Thresholds are deliberately loose -- the claim is "this toggle reaches the screen", and the
 * actual counts print on failure so a real change in coverage is easy to re-baseline.
 */
COOPA_TEST(frame_is_reproducible_and_live_toggles_reach_it) {
    ScopedEnv fixed_dt("FIXED_DT", "0");   // freeze time: see this file's doc
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/demos/pixel_demo/scene.yaml", 640, 360, 160, 90);
    config.render.palette_path    = "assets/palettes/pico8.png";
    config.render.dither_strength = 0.08f;
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, kNoiseCycle); // frame 0 has no temporal history of any kind

    const Frame palette_frame = engine.capture_image(/*low_res=*/true);
    expect(palette_frame.width == 160 && palette_frame.height == 90,
           "headless render: low-res buffer has the configured dimensions");
    expect(palette_frame.channels == 4, "headless render: capture_image returns 4 bytes/texel");

    const long long off_palette =
        first_off_palette_pixel(palette_frame, kPico8Subset, std::size(kPico8Subset));
    expect(off_palette < 0, "headless render: every output pixel matches a palette entry");
    if (off_palette >= 0) {
        const size_t i = static_cast<size_t>(off_palette) * palette_frame.channels;
        std::cerr << "         first off-palette pixel " << off_palette << " = ("
                  << int(palette_frame.pixels[i]) << ", " << int(palette_frame.pixels[i + 1])
                  << ", " << int(palette_frame.pixels[i + 2]) << ")\n";
        dump_frame(palette_frame, "pixel_demo_off_palette");
    }

    // The load-bearing assertion for every diff below it.
    tick_frames(engine, kNoiseCycle);
    const Frame repeat = engine.capture_image(true);
    const long long drift = count_diff(repeat, palette_frame);
    expect(drift <= kDriftBudget,
           "headless render: with FIXED_DT=0, two captures a noise cycle apart are the same frame");
    if (drift > kDriftBudget) std::cerr << "         drift = " << drift << " px\n";
    if (drift > kDriftBudget) {
        std::cerr << "         " << drift << " pixels drifted with nothing changed -- every"
                     " threshold below is unreliable until this passes\n";
        dump_frame(palette_frame, "pixel_demo_frame_a");
        dump_frame(repeat, "pixel_demo_frame_b");
    }

    // --- palette_enabled / dither_enabled ---
    engine.render_config().palette_enabled = false;
    engine.render_config().dither_enabled  = false;
    tick_frames(engine, kNoiseCycle);
    const Frame unquantized = engine.capture_image(true);
    expect_at_least(count_diff(unquantized, palette_frame), 1000,
                    "palette_enabled + dither_enabled toggle measurably changes the rendered frame");
    expect(first_off_palette_pixel(unquantized, kPico8Subset, std::size(kPico8Subset)) >= 0,
           "palette_enabled=false lets the output leave the palette (so the check above means something)");

    // --- sdf_enabled: the demo has an opaque SdfRenderer blob and a BLEND sphere ---
    engine.render_config().sdf_enabled = false;
    tick_frames(engine, kNoiseCycle);
    const Frame no_sdf = engine.capture_image(true);
    const long long sdf_diff = count_diff(no_sdf, unquantized);
    expect_at_least(sdf_diff, 200, "sdf_enabled toggle measurably changes the rendered frame");
    if (sdf_diff < 200) {
        dump_frame(unquantized, "pixel_demo_sdf_on");
        dump_frame(no_sdf, "pixel_demo_sdf_off");
    }

    // --- outline_enabled: edge detect over the G-buffer, so it changes silhouettes only ---
    engine.render_config().outline_enabled = false;
    tick_frames(engine, kNoiseCycle);
    const Frame no_outline = engine.capture_image(true);
    const long long outline_diff = count_diff(no_outline, no_sdf);
    expect_at_least(outline_diff, 100, "outline_enabled toggle measurably changes the rendered frame");
    if (outline_diff < 100) {
        dump_frame(no_sdf, "pixel_demo_outline_on");
        dump_frame(no_outline, "pixel_demo_outline_off");
    }
}

/**
 * @brief Headless render with every STARTUP-FIXED toggle off, which is the only way to cover
 * their "off" construction branch.
 *
 * This one keeps an Engine of its own on purpose. Unlike the live toggles above, these
 * decisions are baked into descriptors when the pipeline is built (see
 * pixel_render_pipeline.h's rule 1), so the code below only ever runs in a pipeline
 * constructed this way: SsaoPass::invalidate_history() instead of execute(), no
 * HiZPass/SceneColorMipPass/SsrPass construction at all, the manual gbuffer-depth transition
 * instead of HiZPass's, pixel_stylize_pass_ reading offscreen_target_ directly instead of
 * ssr_pass_'s composite output, and neither UI pipeline nor the scene-depth descriptor built.
 *
 * A non-black frame of the right size is a low bar and deliberately so -- it is exactly what
 * an all-zero G-buffer read, a missing descriptor bind or a null pass pointer in one of those
 * branches would fail.
 */
COOPA_TEST(startup_toggles_off_still_render) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    toy::core::AppConfig config =
        make_test_config("assets/scenes/demos/pixel_demo/scene.yaml", 640, 360, 160, 90);
    config.render.outline_enabled   = false;
    config.render.palette_enabled   = false;
    config.render.dither_enabled    = false;
    config.render.ssao_enabled      = false;
    config.render.ssr_enabled       = false;
    config.render.world_ui_enabled  = false;
    config.render.screen_ui_enabled = false;

    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 3);
    const Frame frame = engine.capture_image(true);

    expect(frame.width == 160 && frame.height == 90,
           "all-toggles-off headless render: low-res buffer has the configured dimensions");

    const long long lit = count_nonblack(frame);
    expect_at_least(lit, 1, "all-toggles-off headless render: output is not entirely black");
    if (lit == 0) dump_frame(frame, "all_toggles_off_black");
}

/**
 * @brief pixel_demo packaged to .caml (scene, meshes, LOD sidecars) renders byte-identically to
 *        the YAML original -- the end-to-end claim behind Build > Package.
 */
COOPA_TEST(caml_packaged_scene_renders_identically) {
    toy::core::install_caml_codec();   // what the game's main() does before loading anything
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    const std::filesystem::path packed = coopa::test::scratch_dir("caml_render") / "pixel_demo";
    package_tree_as_caml(std::string(ROOT_DIR) + "/assets/scenes/demos/pixel_demo", packed);

    auto render = [](const std::string& scene) {
        toy::core::Engine engine(make_test_config(scene, 320, 180, 160, 90));
        tick_frames(engine, kNoiseCycle);
        return engine.capture_image(/*low_res=*/true);
    };
    const Frame yaml_frame = render("assets/scenes/pixel_demo/scene.yaml");
    const Frame caml_frame = render((packed / "scene.yaml").string());
    const long long diff = count_diff(yaml_frame, caml_frame);
    expect(diff == 0, "pixel_demo from .caml renders identically to .yaml (" + std::to_string(diff) + " px differ)");
    if (diff != 0) {
        dump_frame(yaml_frame, "caml_render_yaml");
        dump_frame(caml_frame, "caml_render_caml");
    }
}
