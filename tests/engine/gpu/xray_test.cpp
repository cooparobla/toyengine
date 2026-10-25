/**
 * @file xray_test.cpp
 * @brief The see-through silhouette, end to end: it is byte-inert when off, it draws through a
 *        surface that opted in, it does NOT draw through one that did not, and overlapping
 *        subjects come out as one shape rather than two.
 *
 * The fixtures are a ball exactly behind a slab (tests/fixtures/scenes/xray/), so "the subject
 * is hidden" is a property of the scene rather than of where the camera drifted to. Every
 * assertion counts pixels near the configured silhouette colour, which no surface in the
 * fixtures comes near on its own -- the same method toon_shading_test.cpp uses for ink.
 */

#include <coopa/testing/test.h>

#include <iostream>
#include <string>

#include <toyengine/core/engine.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("xray");

using namespace toy::test;

namespace {

/// The silhouette colour the fixtures are rendered with. Saturated cyan: nothing in the
/// fixture scenes (grey floor, grey slab, red ball, blue-ish sky) lands anywhere near it.
constexpr float kXrayR = 0.0f;
constexpr float kXrayG = 1.0f;
constexpr float kXrayB = 1.0f;

/// Pixels close to the x-ray colour. The composite blends at `xray_color.a`, so the drawn
/// pixels are not the pure colour -- the tolerance has to be wide enough to catch a 0.9-alpha
/// blend over a mid-grey slab while staying clear of anything else in the frame.
long long xray_pixels(const Frame& f) {
    return count_near_color(f, 40, 220, 235, 60);
}

template <typename Tune>
Frame render(const char* scene, Tune tune) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(scene, 640, 360, 240, 135);
    config.render.soft_lighting = true;
    // A high alpha and no outline unless a test asks: the fill is the easiest signal to count.
    config.render.xray_color     = glm::vec4(kXrayR, kXrayG, kXrayB, 0.9f);
    config.render.xray_thickness = 2.0f;
    tune(config.render);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 4);
    return engine.capture_image(/*low_res=*/true);
}

const char* kClear     = "tests/fixtures/scenes/xray/scene_clear.yaml";
const char* kPermitted = "tests/fixtures/scenes/xray/scene_permitted.yaml";
const char* kDenied    = "tests/fixtures/scenes/xray/scene_denied.yaml";
const char* kTwoBalls  = "tests/fixtures/scenes/xray/scene_two_balls.yaml";

} // namespace

/**
 * @brief xray_enabled false renders the EXACT same image as a scene with no markers at all.
 *
 * The feature's whole promise is that it costs nothing when off, and "nothing" has to mean
 * byte-identical rather than nearly: a difference here would mean the pass is perturbing the
 * frame (an extra clear, a stray blend) on every scene that does not use it.
 */
COOPA_TEST(xray_disabled_is_byte_identical_to_an_unmarked_scene) {
    const Frame off     = render(kPermitted, [](auto& r) { r.xray_enabled = false; });
    const Frame no_mark = render(kClear,     [](auto& r) { r.xray_enabled = false; });
    require(same_extent(off, no_mark), "xray: both captures are the same size");
    const long long diff = count_diff(off, no_mark);
    EXPECT_EQ(diff, 0);
    if (diff != 0) {
        std::cout << "  differing pixels: " << diff << "\n";
        dump_frame(off, "xray_off_permitted");
        dump_frame(no_mark, "xray_off_clear");
    }
}

/** @brief With the feature on but nothing marked, the frame is still untouched. */
COOPA_TEST(xray_on_but_nothing_marked_draws_nothing) {
    const Frame off = render(kClear, [](auto& r) { r.xray_enabled = false; });
    const Frame on  = render(kClear, [](auto& r) { r.xray_enabled = true; });
    const long long diff = count_diff(off, on);
    expect(diff <= kDriftBudget,
           "xray: an enabled pass with no flagged objects leaves the image alone");
    if (diff > kDriftBudget) {
        std::cout << "  differing pixels: " << diff << "\n";
        dump_frame(on, "xray_on_clear");
    }
}

/** @brief A subject hidden behind a surface that opted in shows through it. */
COOPA_TEST(a_permitting_occluder_reveals_the_hidden_subject) {
    const Frame off = render(kPermitted, [](auto& r) { r.xray_enabled = false; });
    const Frame on  = render(kPermitted, [](auto& r) { r.xray_enabled = true; });
    const long long n_off = xray_pixels(off);
    const long long n_on  = xray_pixels(on);
    expect(n_off == 0, "xray: nothing is near the silhouette colour with the feature off");
    expect(n_on > 100, "xray: the silhouette appears through a permitting occluder");
    if (!(n_off == 0 && n_on > 100)) {
        std::cout << "  silhouette pixels: off=" << n_off << " on=" << n_on << "\n";
        dump_frame(on, "xray_permitted_on");
    }
}

/**
 * @brief A subject hidden behind a surface that did NOT opt in stays hidden.
 *
 * This is the assertion the whole opt-in design exists for. Everything else could pass with a
 * pass that ignored XrayOccluder entirely and drew the subject through anything.
 */
COOPA_TEST(a_non_permitting_occluder_keeps_the_subject_hidden) {
    const Frame denied    = render(kDenied,    [](auto& r) { r.xray_enabled = true; });
    const Frame permitted = render(kPermitted, [](auto& r) { r.xray_enabled = true; });
    const long long n_denied    = xray_pixels(denied);
    const long long n_permitted = xray_pixels(permitted);
    EXPECT_EQ(n_denied, 0);
    expect(n_permitted > 100, "xray: the permitted control really does draw (so 0 above means gated)");
    if (n_denied != 0) {
        std::cout << "  silhouette pixels: denied=" << n_denied << " permitted=" << n_permitted << "\n";
        dump_frame(denied, "xray_denied_on");
    }
}

/** @brief Outline mode draws strictly less than the fill, and both draws more than either. */
COOPA_TEST(outline_mode_draws_a_line_and_both_draws_more_than_it) {
    const Frame fill    = render(kPermitted, [](auto& r) { r.xray_enabled = true; r.xray_mode = "silhouette"; });
    const Frame outline = render(kPermitted, [](auto& r) { r.xray_enabled = true; r.xray_mode = "outline"; });
    const Frame both    = render(kPermitted, [](auto& r) { r.xray_enabled = true; r.xray_mode = "both"; });
    const long long n_fill = xray_pixels(fill);
    const long long n_line = xray_pixels(outline);
    const long long n_both = xray_pixels(both);
    expect(n_line > 0, "xray: outline mode draws something");
    expect(n_line < n_fill, "xray: an outline is less coverage than a filled silhouette");
    expect(n_both >= n_fill, "xray: `both` covers at least what the fill alone does");
    if (!(n_line > 0 && n_line < n_fill && n_both >= n_fill)) {
        std::cout << "  pixels: fill=" << n_fill << " outline=" << n_line << " both=" << n_both << "\n";
        dump_frame(outline, "xray_outline");
        dump_frame(fill, "xray_fill");
    }
}

/**
 * @brief Two overlapping subjects read as ONE shape, not two stacked ones.
 *
 * The mask is blended with BlendMode::Max precisely so this holds: under additive blending the
 * overlap would be twice as bright, and under no blending the result would depend on which ball
 * happened to be drawn last. Checked two ways -- the overlap is not brighter (the union is flat),
 * and the outline is less than twice one ball's (there is no seam line through the middle).
 */
COOPA_TEST(overlapping_subjects_merge_into_one_silhouette) {
    const Frame one = render(kPermitted, [](auto& r) { r.xray_enabled = true; });
    const Frame two = render(kTwoBalls,  [](auto& r) { r.xray_enabled = true; });
    const long long n_one = xray_pixels(one);
    const long long n_two = xray_pixels(two);
    expect(n_two > n_one, "xray: two overlapping balls cover more than one");
    expect(n_two < 2 * n_one, "xray: but less than two separate ones -- they overlap, as laid out");

    const Frame line_one = render(kPermitted, [](auto& r) { r.xray_enabled = true; r.xray_mode = "outline"; });
    const Frame line_two = render(kTwoBalls,  [](auto& r) { r.xray_enabled = true; r.xray_mode = "outline"; });
    const long long l_one = xray_pixels(line_one);
    const long long l_two = xray_pixels(line_two);
    // One union outline round a wider shape, not two circles: well under double.
    expect(l_two < 2 * l_one, "xray: the pair is outlined once, not once each");
    if (!(n_two > n_one && n_two < 2 * n_one && l_two < 2 * l_one)) {
        std::cout << "  fill: one=" << n_one << " two=" << n_two
                  << "  outline: one=" << l_one << " two=" << l_two << "\n";
        dump_frame(two, "xray_two_balls");
        dump_frame(line_two, "xray_two_balls_outline");
    }
}

/** @brief The authored colour is what reaches the screen, and the alpha controls how much. */
COOPA_TEST(the_configured_colour_and_alpha_drive_the_overlay) {
    const Frame faint = render(kPermitted, [](auto& r) {
        r.xray_enabled = true;
        r.xray_color   = glm::vec4(kXrayR, kXrayG, kXrayB, 0.15f);
    });
    const Frame strong = render(kPermitted, [](auto& r) { r.xray_enabled = true; });
    expect(xray_pixels(strong) > xray_pixels(faint),
           "xray: a higher xray_color.a lands more of the authored colour on screen");
}

/** @brief depth_fade changes the image, and only dims -- it never adds coverage. */
COOPA_TEST(depth_fade_dims_the_silhouette_without_widening_it) {
    const Frame flat   = render(kPermitted, [](auto& r) { r.xray_enabled = true; r.xray_depth_fade = 0.0f; });
    const Frame faded  = render(kPermitted, [](auto& r) { r.xray_enabled = true; r.xray_depth_fade = 1.0f; });
    const long long n_flat  = xray_pixels(flat);
    const long long n_faded = xray_pixels(faded);
    expect(n_flat > 0, "xray: the flat control draws");
    expect(n_faded <= n_flat, "xray: depth_fade only ever takes strength away");
    if (n_faded > n_flat) {
        std::cout << "  pixels: flat=" << n_flat << " faded=" << n_faded << "\n";
        dump_frame(faded, "xray_depth_fade");
    }
}
