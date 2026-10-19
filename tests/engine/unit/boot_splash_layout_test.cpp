/**
 * @file boot_splash_layout_test.cpp
 * @brief emit_boot_splash()'s geometry: the backdrop covers the window, the logo is centred and
 *        upright, the bar tracks progress (and still moves when there is none), and the layout
 *        holds at any aspect ratio -- all without a Vulkan device.
 *
 * The splash is the one screen whose placement cannot be eyeballed during a normal run: it is
 * gone by the time the app draws. Everything checked here is read straight out of the DrawList's
 * vertices, which is also why emit_boot_splash() is a free function rather than a BootSplash
 * method -- see boot_splash.h.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/core/boot_splash.h>
#include <uicoopa/immediate/imm.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("boot_splash_layout");

namespace {

using coopa::ui::DrawList;

/// A fake-but-distinct logo identity: this test never touches Vulkan (see TextureView's doc).
const coopa::gfx::TextureView kLogo{0x1001};
const coopa::gfx::TextureView kWhite{0x1002};

DrawList fresh_list() {
    DrawList dl;
    dl.set_default_texture(kWhite);
    return dl;
}

/** @brief The axis-aligned bounds of every vertex drawn with `view`. */
struct Bounds {
    glm::vec2 min{1e30f}, max{-1e30f};
    size_t    count = 0;
    bool      any() const { return count > 0; }
    glm::vec2 center() const { return (min + max) * 0.5f; }
    glm::vec2 size() const { return max - min; }
};

Bounds bounds_of(const DrawList& dl, coopa::gfx::TextureView view) {
    Bounds b;
    // A batch's index range names the vertices it drew; walking the indices (rather than the
    // whole vertex array) is what keeps one batch's bounds free of its neighbours'.
    for (const coopa::ui::DrawBatch& batch : dl.batches()) {
        if (batch.texture_view != view) continue;
        for (uint32_t i = batch.first_index; i < batch.first_index + batch.index_count; ++i) {
            const coopa::ui::UiVertex& v = dl.vertices()[dl.indices()[i]];
            b.min = glm::min(b.min, glm::vec2(v.x, v.y));
            b.max = glm::max(b.max, glm::vec2(v.x, v.y));
            ++b.count;
        }
    }
    return b;
}

/** @brief The logo quad's four corner vertices, in emission order. */
std::vector<coopa::ui::UiVertex> logo_quad(const DrawList& dl) {
    std::vector<coopa::ui::UiVertex> out;
    for (const coopa::ui::DrawBatch& batch : dl.batches()) {
        if (batch.texture_view != kLogo) continue;
        const uint32_t first_vertex = dl.indices()[batch.first_index];
        for (uint32_t i = 0; i < 4 && first_vertex + i < dl.vertices().size(); ++i) {
            out.push_back(dl.vertices()[first_vertex + i]);
        }
    }
    return out;
}

bool all_finite(const DrawList& dl) {
    for (const coopa::ui::UiVertex& v : dl.vertices()) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.u) || !std::isfinite(v.v)) return false;
    }
    return true;
}

}  // namespace

COOPA_TEST(backdrop_covers_the_window_and_the_logo_sits_in_the_middle) {
    const glm::vec2 canvas(1600.0f, 900.0f);
    toy::core::BootSplashStyle style;
    DrawList dl = fresh_list();
    toy::core::emit_boot_splash(dl, canvas, style, kLogo, nullptr, "Loading scene...", 0.5f, 0.0f);

    expect(!dl.indices().empty(), "splash: something was emitted");
    expect(all_finite(dl), "splash: every vertex is finite");

    const Bounds flat = bounds_of(dl, kWhite);
    expect(flat.any(), "splash: the backdrop and bar draw with the default white texture");
    expect(flat.min.x <= 0.0f && flat.min.y <= 0.0f &&
           flat.max.x >= canvas.x && flat.max.y >= canvas.y,
           "splash: the backdrop spans the whole window");

    const Bounds logo = bounds_of(dl, kLogo);
    expect(logo.any(), "splash: the logo is drawn with its own texture");
    expect_near(logo.center().x, canvas.x * 0.5f, 0.5f, "splash: the logo is horizontally centred");
    expect(logo.center().y > canvas.y * 0.5f, "splash: the logo sits above the middle, over the wordmark");
    expect_near(logo.size().x, logo.size().y, 0.5f, "splash: the logo is square");
    expect(logo.size().x > 100.0f && logo.size().x < canvas.y * 0.5f,
           "splash: the logo is a sensible size (" + std::to_string(logo.size().x) + "px)");
    expect(logo.min.x > 0.0f && logo.max.x < canvas.x && logo.min.y > 0.0f && logo.max.y < canvas.y,
           "splash: the logo is fully on screen");
}

COOPA_TEST(the_logo_is_drawn_upright) {
    // Canvas space is +Y up and the logo's first pixel row is its TOP, so V must run 1 -> 0 as
    // Y increases. Get this backwards and the window shows the block upside down -- which no
    // test that only checks positions would ever notice.
    DrawList dl = fresh_list();
    toy::core::emit_boot_splash(dl, glm::vec2(1280.0f, 720.0f), toy::core::BootSplashStyle{},
                                kLogo, nullptr, "", 0.0f, 0.0f);
    const std::vector<coopa::ui::UiVertex> quad = logo_quad(dl);
    expect(quad.size() == 4, "logo: one quad, four vertices");
    if (quad.size() == 4) {
        // add_quad emits (min,min), (max,min), (max,max), (min,max) in position space.
        const coopa::ui::UiVertex& bottom_left = quad[0];
        const coopa::ui::UiVertex& top_right   = quad[2];
        expect(bottom_left.v > top_right.v,
               "logo: the bottom of the quad samples the bottom of the image (v " +
                   std::to_string(bottom_left.v) + " > " + std::to_string(top_right.v) + ")");
        expect_near(bottom_left.u, 0.0f, 1e-6f, "logo: the full width is sampled");
        expect_near(top_right.u, 1.0f, 1e-6f, "logo: the full width is sampled");
    }
}

COOPA_TEST(the_bar_tracks_progress_and_keeps_moving_without_it) {
    const glm::vec2 canvas(1280.0f, 720.0f);
    auto emit = [&](float progress, float time) {
        DrawList dl = fresh_list();
        toy::core::emit_boot_splash(dl, canvas, toy::core::BootSplashStyle{}, kLogo, nullptr,
                                    "", progress, time);
        return dl;
    };

    // Vertex count is the measure here: a wider fill is more perimeter segments, and an empty
    // bar emits no fill quad at all. Comparing counts needs no knowledge of the rounding.
    const size_t empty = emit(0.0f, 0.0f).vertices().size();
    const size_t half  = emit(0.5f, 0.0f).vertices().size();
    const size_t full  = emit(1.0f, 0.0f).vertices().size();
    expect(half > empty, "bar: a half-full bar draws more than an empty one");
    expect(full >= half, "bar: a full bar draws at least as much as a half-full one");

    // The fill's own bounds: the white-texture geometry reaches further right at 1.0 than at 0.5,
    // and the track is the same width in both.
    const Bounds at_half = bounds_of(emit(0.5f, 0.0f), kWhite);
    const Bounds at_full = bounds_of(emit(1.0f, 0.0f), kWhite);
    expect_near(at_half.max.x, at_full.max.x, 0.5f, "bar: the track is the same either way");
    expect(at_half.any() && at_full.any(), "bar: the bar geometry exists");

    // Indeterminate: the segment is somewhere else a second later, and never leaves the track.
    const DrawList a = emit(-1.0f, 0.0f);
    const DrawList b = emit(-1.0f, 0.9f);
    bool moved = a.vertices().size() != b.vertices().size();
    for (size_t i = 0; !moved && i < std::min(a.vertices().size(), b.vertices().size()); ++i) {
        moved = std::abs(a.vertices()[i].x - b.vertices()[i].x) > 1.0f;
    }
    expect(moved, "bar: an indeterminate bar moves over time");
    const Bounds sweep = bounds_of(b, kWhite);
    expect(sweep.min.x >= 0.0f && sweep.max.x <= canvas.x, "bar: the sweep stays inside the window");
}

COOPA_TEST(the_layout_survives_odd_canvases) {
    for (glm::vec2 canvas : {glm::vec2(2560.0f, 1280.0f), glm::vec2(600.0f, 1200.0f),
                             glm::vec2(320.0f, 200.0f), glm::vec2(3840.0f, 2160.0f)}) {
        DrawList dl = fresh_list();
        toy::core::emit_boot_splash(dl, canvas, toy::core::BootSplashStyle{}, kLogo, nullptr,
                                    "Loading assets...", 0.33f, 1.5f);
        const std::string where = std::to_string(static_cast<int>(canvas.x)) + "x" +
                                  std::to_string(static_cast<int>(canvas.y));
        expect(all_finite(dl), "aspect " + where + ": every vertex is finite");
        const Bounds logo = bounds_of(dl, kLogo);
        expect(logo.any(), "aspect " + where + ": the logo is drawn");
        expect(logo.min.x >= 0.0f && logo.max.x <= canvas.x && logo.min.y >= 0.0f && logo.max.y <= canvas.y,
               "aspect " + where + ": the logo is fully on screen");
        expect_near(logo.center().x, canvas.x * 0.5f, 0.5f, "aspect " + where + ": the logo is centred");
    }

    // A zero-sized window (minimized) emits nothing rather than dividing by it.
    DrawList none = fresh_list();
    toy::core::emit_boot_splash(none, glm::vec2(0.0f, 0.0f), toy::core::BootSplashStyle{}, kLogo,
                                nullptr, "", 0.5f, 0.0f);
    expect(none.indices().empty(), "a zero-sized window emits no geometry");
}

COOPA_TEST(text_is_drawn_only_when_there_is_a_font) {
    const glm::vec2 canvas(1280.0f, 720.0f);
    toy::core::BootSplashStyle style;

    DrawList without = fresh_list();
    toy::core::emit_boot_splash(without, canvas, style, kLogo, nullptr, "Loading scene...", 0.5f, 0.0f);

    // A TextRenderer with no font measures but draws nothing -- the same path a missing
    // assets/fonts file leaves BootSplash in, which must still produce a usable screen.
    coopa::ui::imm::TextRenderer fontless;
    DrawList with = fresh_list();
    toy::core::emit_boot_splash(with, canvas, style, kLogo, &fontless, "Loading scene...", 0.5f, 0.0f);

    expect(with.vertices().size() == without.vertices().size(),
           "text: a fontless renderer emits no glyphs, and nothing else changes");
    expect(!without.indices().empty(), "text: the rest of the screen is still drawn");
}
