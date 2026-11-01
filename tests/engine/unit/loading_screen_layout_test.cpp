/**
 * @file loading_screen_layout_test.cpp
 * @brief emit_loading_screen()'s geometry: the mark sits in the chosen corner, scales with
 *        canvas height, draws nothing when disabled, and animates periodically -- all without
 *        a Vulkan device.
 *
 * Modelled on boot_splash_layout_test.cpp: the mark is gone by the time anything else draws
 * (it is part of a transition that disappears once a load finishes), so everything here is
 * read straight out of the DrawList's vertices.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/core/loading_screen.h>
#include <uicoopa/render/draw_list.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("loading_screen_layout");

namespace {

using coopa::ui::DrawList;
using coopa::ui::Rect;
using toy::core::LoadingConfig;
using toy::core::LoadingCorner;

const coopa::gfx::TextureView kWhite{0x2001};

DrawList fresh_list(glm::vec2 canvas) {
    DrawList dl;
    dl.set_default_texture(kWhite);
    dl.begin(Rect{glm::vec2(0.0f), canvas});
    return dl;
}

struct Bounds {
    glm::vec2 min{1e30f}, max{-1e30f};
    size_t    count = 0;
    bool      any() const { return count > 0; }
    glm::vec2 center() const { return (min + max) * 0.5f; }
    glm::vec2 size() const { return max - min; }
};

Bounds bounds_of(const DrawList& dl) {
    Bounds b;
    for (const coopa::ui::UiVertex& v : dl.vertices()) {
        b.min = glm::min(b.min, glm::vec2(v.x, v.y));
        b.max = glm::max(b.max, glm::vec2(v.x, v.y));
        ++b.count;
    }
    return b;
}

bool all_finite(const DrawList& dl) {
    for (const coopa::ui::UiVertex& v : dl.vertices()) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y)) return false;
    }
    return true;
}

}  // namespace

COOPA_TEST(spinner_false_draws_nothing) {
    LoadingConfig style;
    style.spinner = false;
    DrawList dl = fresh_list({1280.0f, 720.0f});
    toy::core::emit_loading_screen(dl, {1280.0f, 720.0f}, style, 0.0f);
    expect(dl.vertices().empty(), "spinner off: no geometry at all");
}

COOPA_TEST(spinner_true_draws_three_flat_coloured_faces) {
    LoadingConfig style;
    const glm::vec2 canvas(1280.0f, 720.0f);
    DrawList dl = fresh_list(canvas);
    toy::core::emit_loading_screen(dl, canvas, style, 0.0f);

    expect(!dl.indices().empty(), "mark: something was emitted");
    expect(all_finite(dl), "mark: every vertex is finite");
    // Every triangle samples the texture set_default_texture() left bound -- nothing here
    // switches textures, so there is exactly one batch and it is the default one, i.e. flat
    // colour rather than a glyph or a sprite.
    expect(dl.batches().size() == 1 && dl.batches()[0].texture_view == kWhite,
           "mark: flat-coloured -- one batch, the default texture");
    // Three faces, two triangles each (add_triangle has no index reuse).
    expect(dl.indices().size() == 18, "mark: three faces, two triangles each");
}

COOPA_TEST(mark_sits_in_the_chosen_corner) {
    const glm::vec2 canvas(1600.0f, 900.0f);
    struct Case { LoadingCorner corner; bool right; bool top; };
    const Case cases[] = {
        {LoadingCorner::BottomRight, true,  false},
        {LoadingCorner::BottomLeft,  false, false},
        {LoadingCorner::TopRight,    true,  true},
        {LoadingCorner::TopLeft,     false, true},
    };
    for (const Case& c : cases) {
        LoadingConfig style;
        style.corner = c.corner;
        DrawList dl = fresh_list(canvas);
        toy::core::emit_loading_screen(dl, canvas, style, 0.0f);
        const Bounds b = bounds_of(dl);
        expect(b.any(), "corner case: something drew");
        // Canvas space is +Y up, so "top" is the larger-Y half.
        expect((b.center().x > canvas.x * 0.5f) == c.right, "corner: X half matches");
        expect((b.center().y > canvas.y * 0.5f) == c.top, "corner: Y half matches");
        expect(b.min.x >= 0.0f && b.max.x <= canvas.x && b.min.y >= 0.0f && b.max.y <= canvas.y,
               "corner: fully on screen");
    }
}

COOPA_TEST(mark_scales_with_canvas_height) {
    LoadingConfig style;
    DrawList small = fresh_list({640.0f, 360.0f});
    toy::core::emit_loading_screen(small, {640.0f, 360.0f}, style, 0.0f);
    DrawList large = fresh_list({3840.0f, 2160.0f});
    toy::core::emit_loading_screen(large, {3840.0f, 2160.0f}, style, 0.0f);

    const Bounds b_small = bounds_of(small);
    const Bounds b_large = bounds_of(large);
    expect(b_small.any() && b_large.any(), "scale: both sizes drew something");
    expect(b_large.size().x > b_small.size().x * 2.0f,
           "scale: the mark is meaningfully bigger on a 4K canvas than a 360p one");
}

COOPA_TEST(the_cycle_is_periodic_and_moves) {
    LoadingConfig style;
    const glm::vec2 canvas(1280.0f, 720.0f);
    auto colors_at = [&](float time) {
        DrawList dl = fresh_list(canvas);
        toy::core::emit_loading_screen(dl, canvas, style, time);
        std::vector<uint32_t> cols;
        for (const coopa::ui::UiVertex& v : dl.vertices()) cols.push_back(v.color);
        return cols;
    };

    const std::vector<uint32_t> at0   = colors_at(0.0f);
    const std::vector<uint32_t> atMid = colors_at(0.4f);
    const std::vector<uint32_t> atPeriod = colors_at(1.1f);   // one full revolution later

    expect(at0 != atMid, "cycle: the faces' brightness changes over time");
    expect(at0 == atPeriod, "cycle: a full revolution returns to the same colours");
}

COOPA_TEST(the_layout_survives_odd_canvases) {
    LoadingConfig style;
    for (glm::vec2 canvas : {glm::vec2(2560.0f, 1280.0f), glm::vec2(600.0f, 1200.0f),
                             glm::vec2(320.0f, 200.0f), glm::vec2(3840.0f, 2160.0f)}) {
        DrawList dl = fresh_list(canvas);
        toy::core::emit_loading_screen(dl, canvas, style, 0.6f);
        const std::string where = std::to_string(static_cast<int>(canvas.x)) + "x" +
                                   std::to_string(static_cast<int>(canvas.y));
        expect(all_finite(dl), "odd canvas " + where + ": every vertex is finite");
        const Bounds b = bounds_of(dl);
        expect(b.min.x >= 0.0f && b.max.x <= canvas.x && b.min.y >= 0.0f && b.max.y <= canvas.y,
               "odd canvas " + where + ": the mark stays on screen");
    }
}
