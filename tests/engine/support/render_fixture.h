#pragma once

/**
 * @file render_fixture.h
 * @brief What every GPU suite (tests/engine/gpu, tests/engine/extended) builds on: a headless
 *        Engine config, frame stepping, and the frame-comparison counters the image assertions
 *        are written in.
 *
 * ## Three conventions every render test follows
 *
 * **1. Nothing visible, nothing grabbed.** `window.visible = false` (see make_test_config())
 * means the window is never mapped, so it cannot appear or take focus, and Engine stands its
 * cursor capture down -- a render test must not steal the pointer of whoever is using the
 * machine while it runs. NO_INPUT=1 completes it: the real keyboard and mouse are ignored, so
 * a frame is the same whatever the desktop is doing.
 *
 * **2. FIXED_DT=0 makes A/B comparisons exact.** With no time advancing and the temporal
 * SSAO/SSR resolves off, two captures of an unchanged config are byte-identical -- which
 * render_pipeline's pixel_demo test asserts outright before trusting any of its diffs. That is
 * what lets these suites compare frames with `== 0` and real area thresholds instead of the
 * invented noise tolerance a wall-clock-driven capture needs. A test that needs time to actually
 * pass (cloth, water, particles, physics) pins FIXED_DT to a real step (1/60) instead -- never
 * wall-clock dt, which headless is a few milliseconds and makes a simulation unreproducible.
 *
 * **3. One Engine per scene, toggles flipped live.** An Engine is a window, a Vulkan device,
 * every pipeline in the frame graph and a fully loaded scene; constructing one to change one
 * bool is the most expensive way to do it. `outline_enabled`, `palette_enabled`,
 * `dither_enabled`, `sdf_enabled`, `debug_view` and friends are re-read per frame, so
 * Engine::render_config() flips them on a live pipeline (see
 * PixelRenderPipeline::render_config_mut()). The STARTUP-FIXED toggles -- ssao/ssr/world_ui/
 * screen_ui and friends -- genuinely cannot be, so a test that covers their "off" construction
 * branch keeps its own Engine.
 *
 * Captures never touch the disk: Engine::capture_image() hands back the pixels, and a PNG is
 * only written when an assertion FAILS (dump_frame()), into the test's scratch directory, whose
 * path is printed.
 */

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

#include <gfxcoopa/util/image_readback.h>

#include <toyengine/core/config.h>

namespace toy::core {
class Engine;
}

namespace toy::test {

using Frame = coopa::gfx::util::ImageData;

/**
 * @brief How many frames apart comparable captures are taken, and how much they may still
 *        differ when nothing changed.
 *
 * Two frame-index-driven noise sources survive FIXED_DT=0, both deliberately (they decorrelate
 * per-pixel noise frame to frame, and neither cares whether time passed): SSAO rotates its noise
 * tile by `frame_index_ & 0x7`, and SSR's interleaved-gradient dither runs on
 * `frame_index_ & 0xFF` -- see ssao_params.noise_rotation and ssr_params.frame_index in
 * PixelRenderPipeline::render(). So "identical" has a period of 256 frames, not 1.
 *
 * Measured on pixel_demo at 160x90: captures 5 frames apart differ by 2 pixels, 8 apart (SSAO's
 * period, which kills the larger source) by 1, and 256 apart by 0 -- but 256 ticks per comparison
 * costs ~20x a whole suite, for one pixel. So captures are spaced by SSAO's cycle and allowed
 * kDriftBudget pixels of SSR dither, a floor 30x below the smallest real signal any toggle
 * assertion looks for (545 pixels, for the SDF toggle).
 */
constexpr int kNoiseCycle  = 8;
constexpr int kDriftBudget = 16; // 0.1% of a 160x90 frame

/**
 * @brief An AppConfig shaped for a headless render test: invisible window, no vsync, no
 *        save-on-exit, and the temporal resolves off.
 *
 * `visible = false` is the one that matters to whoever is using the machine -- see this file's
 * doc. vsync off keeps a tick from sleeping to the monitor's refresh (a 60 Hz cap would make the
 * cloth test's 250-odd ticks take four seconds of pure waiting). The temporal SSAO/SSR resolves
 * are disabled because they accumulate across frames: with them on, two captures at different
 * frame indices differ even when nothing else changed, and every A/B assertion rests on them not
 * doing that.
 *
 * @param scene Scene YAML path, relative to the repo root (or absolute).
 * @param win_w Window width; also the display-resolution capture width.
 * @param win_h Window height.
 * @param rw    Internal render width (the low_res capture's width).
 * @param rh    Internal render height.
 */
toy::core::AppConfig make_test_config(const std::string& scene, uint32_t win_w, uint32_t win_h,
                                      uint32_t rw, uint32_t rh);

/**
 * @brief The shipped render config (assets/config.yaml), with only the window/scene/output bits
 *        a test must own.
 *
 * make_test_config() default-constructs an AppConfig, so it exercises PixelRenderConfig's own
 * defaults -- `aa_mode: "off"`, `dof_enabled: false` -- and NOT what the engine actually ships.
 * That is fine for tests asserting on toggles they set themselves, but it is exactly why a TAA
 * flicker once went unnoticed: nothing in the suite ever rendered with the configuration a user
 * runs. Window is twice the render size, hidden.
 */
toy::core::AppConfig make_shipped_config(const std::string& scene, uint32_t rw, uint32_t rh);

/** @brief Advances an Engine by `frames` ticks. */
void tick_frames(toy::core::Engine& engine, int frames);

/** @brief Ticks until `predicate` holds or the budget runs out; returns the ticks spent. */
int tick_until(toy::core::Engine& engine, int max_frames, const std::function<bool()>& predicate);

/**
 * @brief Writes a captured frame into the running test's scratch directory and prints where.
 *
 * Only ever called on the failure path: an image assertion that fails is nearly impossible to
 * diagnose from a pixel count alone, and equally not worth a file when it passes.
 */
void dump_frame(const Frame& frame, std::string_view name);

/** @brief True if two captures describe the same image geometry (so a per-pixel loop is valid). */
bool same_extent(const Frame& a, const Frame& b);

/**
 * @brief Pixels whose R, G or B differs by more than `tolerance`; -1 if the extents differ.
 *
 * Counts PIXELS, not bytes (a byte count makes a 1-channel shift look like four), and ignores
 * alpha, which is a constant 255 in every target this compares.
 */
long long count_diff(const Frame& a, const Frame& b, int tolerance = 0);

/** @brief Pixels within `tolerance` of an 8-bit RGB colour, per channel. */
long long count_near_color(const Frame& f, int r, int g, int b, int tolerance);

/** @brief Pixels that are not exactly black. */
long long count_nonblack(const Frame& f);

/** @brief Mean per-channel |a - b| over a frame pair (red channel), in 0-255 levels per pixel. */
double mean_abs_delta(const Frame& a, const Frame& b);

/** @brief The first pixel in `f` that matches no entry of `palette`, or -1 if all do. */
long long first_off_palette_pixel(const Frame& f, const uint8_t palette[][3], size_t entries);

/** @brief Mean RGB (0..255) of the rows [y0, y1) and columns [x0, x1) of a frame (fractions). */
glm::vec3 band_mean(const Frame& f, float y0, float y1, float x0 = 0.0f, float x1 = 1.0f);

/** @brief Pixels inside 8 x 8 blocks that are entirely near-black -- the NaN / unwritten signature. */
long long black_block_pixels(const Frame& f);

} // namespace toy::test
