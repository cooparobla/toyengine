/**
 * @file hatch_stylize_test.cpp
 * @brief The stylize.frag crosshatch, the paper grain, the screen/surface hatch_space switch
 *        and the outline's crease fade, on the real deferred path: the hatch stage reads the
 *        graded colour and the G-buffer's world position, and a scene that leaves every new
 *        key at its default renders exactly as it did before these features existed.
 *
 * Nine properties, in the order they would fail if the plumbing broke:
 *   1. hatch_enabled/paper_strength/outline_wobble at 0 render BYTE-IDENTICALLY to a render
 *      with none of the new keys touched at all -- the no-regression guarantee for every
 *      scene authored before hatching existed.
 *   2. hatching only ever darkens the image (it adds ink, never lightens a pixel).
 *   3. the hatch is PERIODIC at the configured spacing, not merely "some noise appeared".
 *   4. more layers means more ink (progressive engagement, not one live layer and two dead
 *      branches).
 *   5. the hatch is keyed to the image's own TONE, not to geometry -- brightening the frame
 *      retreats it.
 *   6. hatch_space screen vs surface take genuinely different code paths (binding 8, the
 *      G-buffer world position, is bound and read).
 *   7. the paper grain perturbs INK COVERAGE only: with hatching on it is zero-mean and
 *      widespread, with every ink stage off it touches nothing at all. That second half is
 *      the regression test for a flat, un-inked sky speckling against the palette quantizer.
 *   8. outline_crease_fade at 0 is byte-identical to the baseline.
 *   9. a live crease fade drops interior ink as a subject recedes while never touching its
 *      silhouette -- the "zoomed-out character fills in solid" fix.
 *
 * Reuses the toon suite's fixture (src/tests/fixtures/scenes/toon/scene_pbr.yaml): a lit
 * sphere over a floor under a soft-shadowed caster already spans dark-to-light, so no new
 * fixture asset is needed.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>
#include <toyengine/core/engine.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("hatch_stylize");

using namespace toy::test;

namespace {

constexpr const char* kScene = "src/tests/fixtures/scenes/toon/scene_pbr.yaml";

/// Renders the fixture, with `tune` given the chance to edit the render config first.
/// soft_lighting forced ON, matching toon_shading_test.cpp's own render() -- this engine's
/// banded-by-default look would otherwise contaminate a comparison that is about hatching.
template <typename Tune>
Frame render(Tune tune) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(kScene, 640, 360, 200, 120);
    config.render.soft_lighting = true;
    tune(config.render);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 3);
    return engine.capture_image(/*low_res=*/true);
}

Frame render_default() {
    return render([](toy::render::ToyRenderConfig&) {});
}

/// scene_pbr.yaml's camera and ball positions (see the fixture's own `camera`/`ball` object
/// blocks) -- the fixed reference this file scales the camera's offset from to simulate
/// distance without touching the fixture or its rotation.
const glm::vec3 kBallPos{0.0f, 0.0f, 1.2f};
const glm::vec3 kCameraNearPos{0.0f, -6.5f, 2.6f};

/// Renders the fixture with the camera pushed `distance_scale` times its default distance from
/// the ball, straight back along the same ray (rotation untouched, so the ball stays centred).
/// Used to prove outline_crease_fade drops a receding subject's INTERIOR ink while leaving
/// its silhouette alone.
template <typename Tune>
Frame render_at_distance(float distance_scale, Tune tune) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config(kScene, 640, 360, 200, 120);
    config.render.soft_lighting = true;
    tune(config.render);
    toy::core::Engine engine(std::move(config));
    auto* camera = engine.scene().find_object("camera");
    if (camera && camera->get_transform()) {
        const glm::vec3 offset = kCameraNearPos - kBallPos;
        camera->get_transform()->transform().set_position(kBallPos + offset * distance_scale);
    }
    tick_frames(engine, 3);
    return engine.capture_image(/*low_res=*/true);
}

/// Pixels within `threshold` (per channel) of the ball's authored albedo (0.85, 0.30, 0.22,
/// sRGB-ish 217,77,56), read off a render with every ink stage OFF -- a simple red-dominance
/// match, not exact colour: ambient/diffuse shading darkens the ball across its surface, but a
/// reddish hue survives shading well enough to separate it from the near-grey floor (0.70,
/// 0.70, 0.72) and the dark grey caster (0.25, 0.25, 0.28). This is the subject-area proxy the
/// world-vs-screen outline comparison normalizes against.
long long count_ball_pixels(const Frame& bare) {
    long long n = 0;
    const size_t count = bare.pixels.size() / 4;
    for (size_t p = 0; p < count; ++p) {
        const int r = bare.pixels[p * 4 + 0];
        const int g = bare.pixels[p * 4 + 1];
        const int b = bare.pixels[p * 4 + 2];
        if (r > g + 20 && r > b + 20) ++n;
    }
    return n;
}

/// Sum, over every ball pixel in `bare` (see count_ball_pixels), of how much darker `inked`
/// made it -- the ink fraction's numerator. A SUM of magnitudes rather than a count of pixels
/// crossing a threshold: the outline's alpha is continuous (outline_coverage's fractional
/// ring, see stylize.frag), so a pixel inked at 30% coverage must count for less than one
/// inked at 100%, not the same "touched" unit -- a binary threshold is blind to exactly the
/// sub-pixel fade this test exists to measure.
double sum_ball_darkening(const Frame& bare, const Frame& inked) {
    double sum = 0.0;
    const size_t count = std::min(bare.pixels.size(), inked.pixels.size()) / 4;
    for (size_t p = 0; p < count; ++p) {
        const int r = bare.pixels[p * 4 + 0];
        const int g = bare.pixels[p * 4 + 1];
        const int b = bare.pixels[p * 4 + 2];
        if (!(r > g + 20 && r > b + 20)) continue;
        const int d = r - static_cast<int>(inked.pixels[p * 4]);
        if (d > 0) sum += d;
    }
    return sum;
}

/// Average display-referred luma over the whole frame, 0..255.
double mean_level(const Frame& f) {
    double sum = 0.0;
    const size_t n = f.pixels.size() / 4;
    for (size_t p = 0; p < n; ++p) {
        const size_t i = p * 4;
        sum += 0.2126 * f.pixels[i] + 0.7152 * f.pixels[i + 1] + 0.0722 * f.pixels[i + 2];
    }
    return n > 0 ? sum / static_cast<double>(n) : 0.0;
}

/// Pixels where `a` is darker than `b` by at least `threshold` levels (R channel; ink is
/// achromatic so one channel is enough), vs. the reverse. Ink should only ever add the first.
void count_darkened_and_lightened(const Frame& a, const Frame& b, int threshold,
                                   long long& darkened, long long& lightened) {
    darkened = 0;
    lightened = 0;
    const size_t n = std::min(a.pixels.size(), b.pixels.size()) / 4;
    for (size_t p = 0; p < n; ++p) {
        const int da = static_cast<int>(a.pixels[p * 4]) - static_cast<int>(b.pixels[p * 4]);
        if (da <= -threshold) ++darkened;
        if (da >= threshold) ++lightened;
    }
}

/// Horizontal transitions of the "a and b differ by >= threshold" mask, row by row -- high for
/// a periodic stroke pattern, low for a uniform tint or a one-off edge.
long long horizontal_transitions(const Frame& a, const Frame& b, int threshold) {
    long long transitions = 0;
    for (uint32_t y = 0; y < a.height; ++y) {
        bool prev = false;
        for (uint32_t x = 0; x < a.width; ++x) {
            const size_t p = static_cast<size_t>(y) * a.width + x;
            const int d = std::abs(static_cast<int>(a.pixels[p * 4]) - static_cast<int>(b.pixels[p * 4]));
            const bool cur = d >= threshold;
            if (x > 0 && cur != prev) ++transitions;
            prev = cur;
        }
    }
    return transitions;
}

/// A generous, deliberately-chosen tone threshold so the hatch engages over most of the frame
/// regardless of how bright the fixture's own lighting happens to be tuned -- these tests are
/// about the hatch mechanism, not about reproducing the demo scene's look.
void enable_hatch(toy::render::ToyRenderConfig& r, float strength, int layers, float scale) {
    r.hatch_enabled  = true;
    r.hatch_strength = strength;
    r.hatch_layers   = layers;
    r.hatch_scale    = scale;
    r.hatch_threshold = 0.9f;
}

}  // namespace

/**
 * @brief hatch_enabled/paper_strength/outline_wobble at 0 render BYTE-IDENTICALLY to a render
 *        that never touches them -- proven for all three independently.
 */
COOPA_TEST(every_new_key_at_zero_is_byte_identical_to_untouched) {
    const Frame baseline = render_default();

    const Frame hatch_off = render([](toy::render::ToyRenderConfig& r) {
        r.hatch_enabled = true;    // the toggle is on, but strength is the no-op value
        r.hatch_strength = 0.0f;
    });
    const Frame paper_off = render([](toy::render::ToyRenderConfig& r) {
        r.paper_strength = 0.0f;
    });
    const Frame wobble_off = render([](toy::render::ToyRenderConfig& r) {
        r.outline_wobble = 0.0f;
    });
    const Frame pbr_thresh_off = render([](toy::render::ToyRenderConfig& r) {
        r.hatch_pbr_threshold = 0.0f;
    });
    const Frame through_water_off = render([](toy::render::ToyRenderConfig& r) {
        r.outline_through_water = 0.0f;
    });

    expect(same_extent(baseline, hatch_off), "hatch: shares the baseline's extent");
    const long long d1 = count_diff(baseline, hatch_off);
    const long long d2 = count_diff(baseline, paper_off);
    const long long d3 = count_diff(baseline, wobble_off);
    expect(d1 == 0, "hatch_strength 0 renders exactly as an untouched config");
    expect(d2 == 0, "paper_strength 0 renders exactly as an untouched config");
    expect(d3 == 0, "outline_wobble 0 renders exactly as an untouched config");
    const long long d4 = count_diff(baseline, pbr_thresh_off);
    const long long d5 = count_diff(baseline, through_water_off);
    expect(d4 == 0, "hatch_pbr_threshold 0 renders exactly as an untouched config");
    expect(d5 == 0, "outline_through_water 0 renders exactly as an untouched config");
    if (d1 != 0 || d2 != 0 || d3 != 0 || d4 != 0 || d5 != 0) {
        std::cout << "  diffs: hatch=" << d1 << " paper=" << d2 << " wobble=" << d3
                  << " pbr_threshold=" << d4 << " through_water=" << d5 << "\n";
        dump_frame(baseline, "hatch_regress_baseline");
        dump_frame(hatch_off, "hatch_regress_hatch");
        dump_frame(paper_off, "hatch_regress_paper");
        dump_frame(wobble_off, "hatch_regress_wobble");
    }
}

/**
 * @brief Hatching only ever adds ink: pixels it changes get darker, essentially never lighter.
 *
 * Both assertions together are what make this a test of INKING rather than of "the config
 * changed the frame" -- a diff alone would pass on a tint in either direction.
 */
COOPA_TEST(hatch_only_ever_adds_ink) {
    const Frame off = render_default();
    const Frame on = render([](toy::render::ToyRenderConfig& r) {
        enable_hatch(r, 1.0f, 2, 4.0f);
    });

    expect(same_extent(off, on), "hatch: shares the baseline's extent");
    if (!same_extent(off, on)) return;

    long long darkened = 0, lightened = 0;
    count_darkened_and_lightened(on, off, 10, darkened, lightened);
    expect_at_least(darkened, 2000, "hatching darkens a meaningful share of the frame");
    expect(darkened > lightened * 20,
           "pixels the hatch touches get darker, essentially none get lighter");
    if (darkened < 2000 || darkened <= lightened * 20) {
        std::cout << "  darkened=" << darkened << " lightened=" << lightened << "\n";
        dump_frame(off, "hatch_ink_off");
        dump_frame(on, "hatch_ink_on");
    }
}

/**
 * @brief The hatch is periodic at the configured spacing: a finer hatch_scale changes far more
 *        often across a row than a coarser one does, over the SAME lit surface.
 *
 * The pair of assertions is the point -- the first says the pattern is periodic rather than a
 * one-off edge or noise, the second says the spacing knob actually controls the period.
 */
COOPA_TEST(hatch_is_periodic_at_the_configured_spacing) {
    const Frame off = render_default();
    const Frame fine = render([](toy::render::ToyRenderConfig& r) {
        enable_hatch(r, 1.0f, 2, 4.0f);
    });
    const Frame coarse = render([](toy::render::ToyRenderConfig& r) {
        enable_hatch(r, 1.0f, 2, 12.0f);
    });

    expect(same_extent(off, fine) && same_extent(off, coarse), "hatch: all three share the extent");
    if (!same_extent(off, fine) || !same_extent(off, coarse)) return;

    const long long fine_transitions = horizontal_transitions(off, fine, 20);
    const long long coarse_transitions = horizontal_transitions(off, coarse, 20);
    expect_at_least(fine_transitions, 150, "a fine hatch changes periodically across the frame");
    expect(fine_transitions > coarse_transitions * 1.3,
           "a finer hatch_scale changes far more often per row than a coarser one");
    if (fine_transitions < 150 || fine_transitions <= coarse_transitions * 1.3) {
        std::cout << "  transitions: fine=" << fine_transitions << " coarse=" << coarse_transitions << "\n";
        dump_frame(fine, "hatch_period_fine");
        dump_frame(coarse, "hatch_period_coarse");
    }
}

/**
 * @brief More hatch layers means more ink: mean frame level strictly decreases as hatch_layers
 *        goes 1 -> 2 -> 3, at fixed strength/threshold/scale.
 *
 * Proves progressive layer engagement, not "one live layer and two dead branches" -- a bug
 * that would leave 2 and 3 indistinguishable from 1.
 */
COOPA_TEST(more_layers_means_more_ink) {
    const Frame l1 = render([](toy::render::ToyRenderConfig& r) { enable_hatch(r, 1.0f, 1, 5.0f); });
    const Frame l2 = render([](toy::render::ToyRenderConfig& r) { enable_hatch(r, 1.0f, 2, 5.0f); });
    const Frame l3 = render([](toy::render::ToyRenderConfig& r) { enable_hatch(r, 1.0f, 3, 5.0f); });

    const double m1 = mean_level(l1);
    const double m2 = mean_level(l2);
    const double m3 = mean_level(l3);

    expect(m1 > m2, "hatch_layers 2 is darker on average than 1");
    expect(m2 > m3, "hatch_layers 3 is darker on average than 2");
    if (!(m1 > m2 && m2 > m3)) {
        std::cout << "  mean level: layers1=" << m1 << " layers2=" << m2 << " layers3=" << m3 << "\n";
        dump_frame(l1, "hatch_layers_1");
        dump_frame(l2, "hatch_layers_2");
        dump_frame(l3, "hatch_layers_3");
    }
}

/**
 * @brief The hatch is keyed to the frame's own TONE, not to geometry: brightening the exposure
 *        retreats the hatched area by more than half.
 *
 * This is the whole luminance-key claim in one number, and it is also the claim a
 * world-space-UV implementation (keyed to a fixed coordinate rather than to rendered
 * brightness) would fail.
 */
COOPA_TEST(hatching_tracks_tone_not_geometry) {
    auto hatched_px = [](float exposure) {
        const Frame off = render([&](toy::render::ToyRenderConfig& r) { r.exposure = exposure; });
        const Frame on = render([&](toy::render::ToyRenderConfig& r) {
            r.exposure = exposure;
            enable_hatch(r, 1.0f, 2, 5.0f);
        });
        return count_diff(off, on, /*tolerance=*/10);
    };

    const long long bright = hatched_px(2.2f);
    const long long dim = hatched_px(0.6f);

    expect_at_least(dim, 2000, "a dim exposure gives the hatch plenty of frame to engage over");
    expect(bright < dim / 2, "brightening the exposure retreats the hatch by more than half");
    if (dim < 2000 || bright >= dim / 2) {
        std::cout << "  hatched px: bright=" << bright << " dim=" << dim << "\n";
    }
}

/**
 * @brief hatch_space screen vs surface produce genuinely different images, and both differ
 *        from the hatch-off baseline -- binding 8 (the G-buffer world position) is bound and
 *        actually read, not a dead parameter.
 */
COOPA_TEST(surface_space_takes_a_different_path_from_screen_space) {
    const Frame off = render_default();
    const Frame screen = render([](toy::render::ToyRenderConfig& r) {
        enable_hatch(r, 1.0f, 2, 5.0f);
        r.hatch_space = "screen";
    });
    const Frame surface = render([](toy::render::ToyRenderConfig& r) {
        enable_hatch(r, 1.0f, 2, 5.0f);
        r.hatch_space = "surface";
    });

    expect(same_extent(screen, surface), "hatch: screen and surface share the extent");
    if (!same_extent(screen, surface)) return;

    const long long screen_vs_surface = count_diff(screen, surface, 10);
    const long long screen_vs_off = count_diff(screen, off, 10);
    const long long surface_vs_off = count_diff(surface, off, 10);

    expect_at_least(screen_vs_surface, 1000, "screen and surface hatch produce different images");
    expect_at_least(screen_vs_off, 2000, "screen-space hatching changes the frame");
    expect_at_least(surface_vs_off, 2000, "surface-space hatching changes the frame");
    if (screen_vs_surface < 1000 || screen_vs_off < 2000 || surface_vs_off < 2000) {
        std::cout << "  screen_vs_surface=" << screen_vs_surface
                  << " screen_vs_off=" << screen_vs_off
                  << " surface_vs_off=" << surface_vs_off << "\n";
        dump_frame(screen, "hatch_space_screen");
        dump_frame(surface, "hatch_space_surface");
    }
}

/**
 * @brief Paper grain modulates ink COVERAGE, not the final colour: with hatching engaged it
 *        touches a substantial number of pixels while barely moving the frame's average level
 *        (zero-mean), and with every ink stage off it touches NONE at all -- the regression test
 *        for a smooth, un-inked region (a sky, a flat wall) speckling on its way into the
 *        palette quantizer.
 */
COOPA_TEST(paper_grain_only_perturbs_ink) {
    {
        const Frame off = render([](toy::render::ToyRenderConfig& r) {
            enable_hatch(r, 0.8f, 2, 5.0f);
        });
        const Frame on = render([](toy::render::ToyRenderConfig& r) {
            enable_hatch(r, 0.8f, 2, 5.0f);
            r.paper_strength = 0.6f;
        });

        expect(same_extent(off, on), "paper: shares the baseline's extent");
        if (!same_extent(off, on)) return;

        const long long touched = count_diff(off, on, 1);
        const double delta = std::abs(mean_level(on) - mean_level(off));

        expect_at_least(touched, 2000, "paper grain touches a substantial share of the ink");
        expect(delta < 1.0, "paper grain barely moves the frame's average level");
        if (touched < 2000 || delta >= 1.0) {
            std::cout << "  touched=" << touched << " mean delta=" << delta << "\n";
        }
    }
    {
        // All ink off: paper_strength must be a true no-op, since there is nothing for the
        // tooth to perturb. This is what keeps a hatch-free, outline-free region -- a Moebius
        // sky, a flat wall -- from speckling against the palette quantizer.
        const Frame off = render([](toy::render::ToyRenderConfig& r) {
            r.hatch_enabled = false;
            r.hatch_strength = 0.0f;
            r.outline_enabled = false;
            r.outline_thickness = 0.0f;
        });
        const Frame on = render([](toy::render::ToyRenderConfig& r) {
            r.hatch_enabled = false;
            r.hatch_strength = 0.0f;
            r.outline_enabled = false;
            r.outline_thickness = 0.0f;
            r.paper_strength = 0.6f;
        });

        const long long touched = count_diff(off, on, 1);
        expect(touched == 0, "paper grain with no ink anywhere touches zero pixels");
        if (touched != 0) {
            std::cout << "  touched=" << touched << "\n";
        }
    }
}

/**
 * @brief outline_crease_fade's default (0) is a pure no-op: every crease still inks at any
 *        distance, so every scene authored before this key existed renders exactly as it did.
 */
COOPA_TEST(crease_fade_off_is_byte_identical_to_the_baseline) {
    const Frame off = render_default();
    const Frame on = render([](toy::render::ToyRenderConfig& r) { r.outline_crease_fade = 0.0f; });

    expect(same_extent(off, on), "crease_fade: shares the baseline's extent");
    if (!same_extent(off, on)) return;

    const long long diff = count_diff(off, on, 0);
    expect(diff == 0, "outline_crease_fade 0 is byte-identical to the untouched baseline");
    if (diff != 0) {
        std::cout << "  diff=" << diff << "\n";
    }
}

/**
 * @brief outline_crease_fade drops INTERIOR ink as a subject recedes while leaving its
 *        SILHOUETTE intact -- the "zoomed-out character fills in solid" fix.
 *
 * Four properties, because no one of them alone would distinguish the feature from simply
 * turning the outline down: an aggressive span removes ink; what survives is still most of the
 * line (the silhouette, which must never fade); a LENIENT span restores the unfaded image
 * exactly, proving the key is read as a texel span rather than as an on/off flag; and for one
 * fixed span, a nearer camera keeps a larger share of its creases than a farther one.
 */
COOPA_TEST(crease_fade_drops_interior_ink_with_distance_but_keeps_the_silhouette) {
    auto bare_cfg = [](toy::render::ToyRenderConfig& r) {
        r.hatch_enabled = false;
        r.hatch_strength = 0.0f;
        r.outline_enabled = false;
        r.outline_thickness = 0.0f;
        r.paper_strength = 0.0f;
    };
    // Pixels this render darkened by a clear margin relative to the ink-free render -- the
    // outline is near-black on a light scene, so 40 levels separates ink from shading drift.
    auto ink_px = [](const Frame& bare, const Frame& inked) {
        long long n = 0;
        const size_t count = std::min(bare.pixels.size(), inked.pixels.size()) / 4;
        for (size_t q = 0; q < count; ++q) {
            if (static_cast<int>(bare.pixels[q * 4]) - static_cast<int>(inked.pixels[q * 4]) >= 40) ++n;
        }
        return n;
    };
    auto inked_at = [](float dist, float fade) {
        return render_at_distance(dist, [fade](toy::render::ToyRenderConfig& r) {
            r.hatch_enabled = false;
            r.hatch_strength = 0.0f;
            r.outline_enabled = true;
            r.outline_thickness = 1.0f;
            r.paper_strength = 0.0f;
            r.outline_crease_fade = fade;
        });
    };

    // A span small enough that one texel already exceeds it at the near pose (so creases are
    // fully faded), one large enough that no texel in the frame reaches it (so none are), and
    // one in between that the near pose clears but the far pose does not.
    constexpr float kAggressive = 0.02f;
    constexpr float kLenient    = 0.3f;
    constexpr float kMiddle     = 0.1f;
    constexpr float kNear = 1.0f;
    constexpr float kFar  = 2.0f;

    const Frame bare_near = render_at_distance(kNear, bare_cfg);
    const long long near_unfaded   = ink_px(bare_near, inked_at(kNear, 0.0f));
    const long long near_aggressive = ink_px(bare_near, inked_at(kNear, kAggressive));
    const long long near_lenient   = ink_px(bare_near, inked_at(kNear, kLenient));
    const long long near_middle    = ink_px(bare_near, inked_at(kNear, kMiddle));

    const Frame bare_far = render_at_distance(kFar, bare_cfg);
    const long long far_unfaded = ink_px(bare_far, inked_at(kFar, 0.0f));
    const long long far_middle  = ink_px(bare_far, inked_at(kFar, kMiddle));

    std::cout << "  near: unfaded=" << near_unfaded << " aggressive=" << near_aggressive
              << " lenient=" << near_lenient << " middle=" << near_middle
              << " | far: unfaded=" << far_unfaded << " middle=" << far_middle << "\n";

    expect_at_least(near_unfaded, 100, "crease_fade: the unfaded render has real ink to measure");
    if (near_unfaded < 100 || far_unfaded <= 0) return;

    expect(near_aggressive < near_unfaded * 9 / 10,
           "crease_fade: a span one texel already exceeds removes interior crease ink");
    expect(near_aggressive > near_unfaded / 2,
           "crease_fade: most of the line survives -- the silhouette is never faded");
    expect(near_lenient == near_unfaded,
           "crease_fade: a span no texel reaches is identical to no fade at all "
           "(the key is a texel span, not an on/off flag)");

    const double near_kept = static_cast<double>(near_middle) / static_cast<double>(near_unfaded);
    const double far_kept  = static_cast<double>(far_middle)  / static_cast<double>(far_unfaded);
    expect(near_kept > far_kept + 0.05,
           "crease_fade: at one fixed span, a nearer camera keeps more of its creases than a farther one");
}

/**
 * @brief `hatch_pbr_threshold` keys the hatch off the MATERIAL, not off the frame: lowering it
 *        pulls ink off un-banded surfaces and leaves toon-banded ones alone.
 *
 * The discriminating measurement is the DIFFERENCE between two scenes, not a count in one.
 * scene_pbr and scene_toon are the same geometry, lighting and camera -- the only difference
 * is that scene_toon's sphere carries toon_bands (see the fixtures' own header comments). So
 * a key that genuinely reads the per-material code must remove more ink from scene_pbr, where
 * every surface is un-banded, than from scene_toon, where the sphere is exempt. A key that
 * merely raised or lowered the hatch globally would take the same amount from both.
 *
 * Both scenes still have an un-banded FLOOR, which is why this asserts an inequality rather
 * than "scene_toon is untouched" -- the floor's ink is correctly dropped in both.
 */
COOPA_TEST(a_pbr_hatch_threshold_spares_toon_banded_materials) {
    const char* kToonScene = "src/tests/fixtures/scenes/toon/scene_toon.yaml";

    const auto render_scene = [](const char* scene, float pbr_threshold) {
        ScopedEnv fixed_dt("FIXED_DT", "0");
        ScopedEnv no_input("NO_INPUT", "1");
        toy::core::AppConfig config = make_test_config(scene, 640, 360, 200, 120);
        config.render.soft_lighting = true;
        enable_hatch(config.render, 0.9f, 3, 6.0f);
        config.render.hatch_pbr_threshold = pbr_threshold;
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, 3);
        return engine.capture_image(/*low_res=*/true);
    };

    // 0 = every surface shares hatch_threshold (0.9, set by enable_hatch). 0.25 is well below
    // it, so an un-banded surface keeps ink only where it is genuinely dark.
    const Frame pbr_shared  = render_scene(kScene, 0.0f);
    const Frame pbr_split   = render_scene(kScene, 0.25f);
    const Frame toon_shared = render_scene(kToonScene, 0.0f);
    const Frame toon_split  = render_scene(kToonScene, 0.25f);

    require(same_extent(pbr_shared, pbr_split) && same_extent(toon_shared, toon_split),
            "pbr threshold: both scenes render at the same extent");

    // Ink REMOVED: the split render is lighter than the shared one wherever hatch was dropped.
    long long pbr_lightened = 0, pbr_darkened = 0;
    long long toon_lightened = 0, toon_darkened = 0;
    count_darkened_and_lightened(pbr_split, pbr_shared, 6, pbr_darkened, pbr_lightened);
    count_darkened_and_lightened(toon_split, toon_shared, 6, toon_darkened, toon_lightened);

    std::cout << "  ink removed: pbr=" << pbr_lightened << " toon=" << toon_lightened << "\n";

    expect(pbr_lightened > 0, "a lowered PBR threshold removes hatch from an un-banded scene");
    expect(pbr_lightened > toon_lightened,
           "it removes MORE from the all-PBR scene than from the one whose sphere is banded");
    expect(pbr_darkened * 4 < pbr_lightened,
           "lowering the threshold only ever removes ink, it does not add any");
}
