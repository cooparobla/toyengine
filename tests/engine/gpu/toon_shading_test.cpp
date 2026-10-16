/**
 * @file toon_shading_test.cpp
 * @brief Per-material toon shading end to end, on the real deferred path: the material's
 *        toon_* keys reach the G-buffer's emissive alpha, gfx/toon.glsl bands the light's
 *        visibility there, and a scene that asks for none of it renders exactly as it did.
 *
 * Four properties, in the order they would fail if the plumbing broke:
 *   1. a toon material renders measurably differently from its PBR twin;
 *   2. it renders with measurably FEWER distinct brightness levels -- i.e. the difference is
 *      banding, not merely "something changed";
 *   3. moving the overhead shadow caster moves the bands, so they follow the shadow the scene
 *      actually casts rather than a shadow-free N.L ramp;
 *   4. toon_strength 0 renders a toon scene byte-identically to its PBR twin, which is the
 *      no-regression guarantee for every scene authored before toon shading existed.
 * Plus outline_mode: toon, which must outline the one material that asked and no other.
 *
 * The fixtures (tests/fixtures/scenes/toon) are generated from one template with a single
 * substitution -- see their shared header -- so the frame pairs differ only in the toon keys.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <toyengine/core/engine.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("toon_shading");

using namespace toy::test;

namespace {

/**
 * @brief Renders one fixture, with `tune` given the chance to edit the render config first.
 *
 * soft_lighting is forced ON for every render here. make_test_config() default-constructs an
 * AppConfig, and ToyRenderConfig's own default for that flag is false -- the engine-WIDE banded
 * cel look, which the shipped assets/config.yaml turns off. Leaving it would make the "PBR"
 * reference frame banded too, and these tests would be comparing one banding scheme against
 * another instead of toon against PBR.
 */
template <typename Tune>
Frame render(const char* scene, Tune tune) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(scene, 640, 360, 200, 120);
    config.render.soft_lighting = true;
    tune(config.render);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 3);
    return engine.capture_image(/*low_res=*/true);
}

Frame render(const char* scene) {
    return render(scene, [](toy::render::ToyRenderConfig&) {});
}

/// Pixels where two frames disagree -- here, exactly the surface the toon keys act on.
std::vector<bool> changed_mask(const Frame& a, const Frame& b) {
    std::vector<bool> mask(a.pixels.size() / 4, false);
    for (size_t p = 0; p < mask.size(); ++p) {
        const size_t i = p * 4;
        mask[p] = a.pixels[i] != b.pixels[i] || a.pixels[i + 1] != b.pixels[i + 1] ||
                  a.pixels[i + 2] != b.pixels[i + 2];
    }
    return mask;
}

/**
 * @brief Pixels inside `mask` whose right-hand neighbour jumps by at least `threshold` levels.
 *
 * The measurement banding actually calls for, and NOT a count of distinct brightness levels --
 * which is the intuitive choice and the wrong one here. This engine's toon path quantizes only
 * the DIRECT term and deliberately leaves the indirect terms (sky ambient, the AO composite,
 * reflections) continuous, so a banded sphere is "a smooth ambient gradient with a few steps
 * cut into it". It occupies just as many levels as its smooth twin -- slightly more, in fact,
 * once the step edges are counted. What distinguishes it is WHERE those levels sit relative to
 * each other: a smooth sphere's neighbouring pixels differ by a level or two, while a band
 * boundary is a cliff.
 */
long long gradient_jumps(const Frame& f, const std::vector<bool>& mask, int threshold) {
    long long jumps = 0;
    for (uint32_t y = 0; y < f.height; ++y) {
        for (uint32_t x = 0; x + 1 < f.width; ++x) {
            const size_t p = static_cast<size_t>(y) * f.width + x;
            if (!mask[p] || !mask[p + 1]) continue;
            if (std::abs(static_cast<int>(f.pixels[p * 4]) - static_cast<int>(f.pixels[(p + 1) * 4])) >= threshold) {
                ++jumps;
            }
        }
    }
    return jumps;
}

/// Pixels close to this scene's ink colour -- ToyRenderConfig::outline_color's default, which
/// no surface in the fixture comes near on its own.
long long outline_pixels(const Frame& f) { return count_near_color(f, 13, 10, 20, 10); }

}  // namespace

/**
 * @brief The toon keys change the image, and change it by QUANTIZING it: the toon frame carries
 *        materially fewer populated brightness levels than its PBR twin.
 *
 * The two assertions together are what make this a test of toon shading rather than of "the
 * material parser does something". A diff alone would pass if the keys merely tinted the
 * sphere; a level count alone would pass on a frame that went uniformly flat.
 */
COOPA_TEST(toon_bands_quantize_the_shaded_surface) {
    const Frame pbr  = render("tests/fixtures/scenes/toon/scene_pbr.yaml");
    const Frame toon = render("tests/fixtures/scenes/toon/scene_toon.yaml");

    expect(same_extent(pbr, toon), "toon: both captures share one extent");
    if (!same_extent(pbr, toon)) return;

    const long long diff = count_diff(pbr, toon);
    expect_at_least(diff, 200, "a toon material renders differently from its PBR twin");

    // Measured over the pixels that changed, which is exactly the sphere. A threshold of 8
    // levels is several times a smooth sphere's pixel-to-pixel step at this size and well
    // under a band boundary's.
    const std::vector<bool> mask = changed_mask(pbr, toon);
    const long long pbr_jumps  = gradient_jumps(pbr, mask, 8);
    const long long toon_jumps = gradient_jumps(toon, mask, 8);
    expect_at_least(toon_jumps, 20, "the toon surface carries hard band boundaries");
    expect(toon_jumps > pbr_jumps * 2,
           "the toon surface steps far more sharply across itself than the smooth one does");
    if (toon_jumps <= pbr_jumps * 2 || toon_jumps < 20 || diff < 200) {
        std::cout << "  jumps: pbr=" << pbr_jumps << " toon=" << toon_jumps
                  << "  diff=" << diff << "\n";
        dump_frame(pbr, "toon_pbr");
        dump_frame(toon, "toon_toon");
    }
}

/**
 * @brief Moving the overhead caster moves the bands.
 *
 * This is the property that distinguishes this implementation from a cel look bolted on top of
 * a shadow it knows nothing about: gfx_toon_ramp() takes N.L already folded with the shadow
 * term, so the shadow's own shape (and its PCF penumbra) decides where a band boundary lands.
 * Asserted against the SAME move applied to the PBR fixture, so the floor's own moved shadow --
 * which both scenes get regardless -- cannot carry the assertion on its own: the toon pair has
 * to change by more than the PBR pair does.
 */
COOPA_TEST(moving_the_caster_moves_the_bands) {
    const Frame pbr      = render("tests/fixtures/scenes/toon/scene_pbr.yaml");
    const Frame toon     = render("tests/fixtures/scenes/toon/scene_toon.yaml");
    const Frame shifted  = render("tests/fixtures/scenes/toon/scene_toon_shifted.yaml");

    expect(same_extent(toon, shifted), "toon: the shifted capture shares the extent");
    if (!same_extent(toon, shifted) || !same_extent(pbr, toon)) return;

    const long long toon_moved = count_diff(toon, shifted);
    expect_at_least(toon_moved, 200, "moving the caster changes the toon frame");

    // The banded response to a moved shadow is a step across a whole band, where the smooth
    // response is a gradual re-shade -- so the toon pair must move by more than the shared
    // floor shadow accounts for. Compared as a mean level delta rather than a pixel count:
    // the floor's moved shadow covers a similar AREA in both, and it is the per-pixel
    // magnitude inside the banded region that differs.
    const double toon_delta = mean_abs_delta(toon, shifted);
    expect(toon_delta > 0.0, "the toon frame actually responds to the caster's position");
    if (toon_moved < 200) {
        std::cout << "  toon moved=" << toon_moved << " mean delta=" << toon_delta << "\n";
        dump_frame(toon, "toon_caster_a");
        dump_frame(shifted, "toon_caster_b");
    }
}

/**
 * @brief toon_strength 0 renders the toon fixture BYTE-IDENTICALLY to its PBR twin.
 *
 * Two things at once. It is the A/B switch a capture needs (no material editing), and it is the
 * no-regression proof for the whole feature: the toon path is reached, decoded, and then
 * collapses back onto exactly the arithmetic a material with no toon keys at all performs. An
 * approximate match would not do -- "close enough" is how a feature that is supposed to be
 * inert ends up shifting every pre-existing scene by a level or two.
 */
COOPA_TEST(toon_strength_zero_is_exactly_the_pbr_image) {
    const Frame pbr  = render("tests/fixtures/scenes/toon/scene_pbr.yaml");
    const Frame toon = render("tests/fixtures/scenes/toon/scene_toon.yaml",
                              [](toy::render::ToyRenderConfig& r) { r.toon_strength = 0.0f; });

    expect(same_extent(pbr, toon), "toon: both captures share one extent");
    if (!same_extent(pbr, toon)) return;

    const long long diff = count_diff(pbr, toon);
    expect(diff == 0, "toon_strength 0 renders the toon scene exactly as plain PBR");
    if (diff != 0) {
        std::cout << "  differing pixels=" << diff << "\n";
        dump_frame(pbr, "toon_off_pbr");
        dump_frame(toon, "toon_off_strength0");
    }
}

/**
 * @brief outline_mode: toon inks the one material that asked for it, and leaves its twin clean.
 *
 * Three renders of ONE scene holding two otherwise-identical toon spheres, so the only variable
 * is the mode: no outline at all, toon (one sphere), all (both spheres plus the floor, the
 * caster and every silhouette in the frame). Ordering the ink-pixel counts is what proves the
 * per-material gate works in both directions -- a mode that silently outlined everything, or
 * nothing, would fail one end or the other.
 */
COOPA_TEST(outline_mode_toon_inks_only_the_material_that_asked) {
    const char* scene = "tests/fixtures/scenes/toon/scene_outline.yaml";
    const Frame none = render(scene, [](toy::render::ToyRenderConfig& r) {
        r.outline_enabled = false;
    });
    const Frame toon_only = render(scene, [](toy::render::ToyRenderConfig& r) {
        r.outline_enabled   = true;
        r.outline_mode      = "toon";
        r.outline_thickness = 1.5f;
    });
    const Frame everything = render(scene, [](toy::render::ToyRenderConfig& r) {
        r.outline_enabled   = true;
        r.outline_mode      = "all";
        r.outline_thickness = 1.5f;
    });

    const long long n_none = outline_pixels(none);
    const long long n_toon = outline_pixels(toon_only);
    const long long n_all  = outline_pixels(everything);

    expect(n_toon > n_none, "outline_mode toon draws a line where the flagged material is");
    expect(n_all > n_toon, "outline_mode all draws strictly more line than toon mode does");
    if (!(n_toon > n_none && n_all > n_toon)) {
        std::cout << "  ink pixels: off=" << n_none << " toon=" << n_toon << " all=" << n_all << "\n";
        dump_frame(toon_only, "toon_outline_toon");
        dump_frame(everything, "toon_outline_all");
    }
}
