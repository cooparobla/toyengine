/**
 * @file boot_splash_layout_test.cpp
 * @brief emit_boot_splash()'s geometry: a flat background fill behind the same cycling mark a
 *        scene load's own cover draws (see loading_screen_layout_test.cpp), plus the
 *        linearize step that is unique to drawing straight into the swapchain's own pass --
 *        all without a Vulkan device.
 *
 * The splash is the one screen whose placement cannot be eyeballed during a normal run: it is
 * gone by the time the app draws. Everything checked here is read straight out of the DrawList's
 * vertices, which is also why emit_boot_splash() is a free function rather than a BootSplash
 * method -- see boot_splash.h.
 */

#include <coopa/testing/test.h>

#include <cmath>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/core/boot_splash.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("boot_splash_layout");

namespace {

using coopa::ui::DrawList;
using toy::core::LoadingConfig;

const coopa::gfx::TextureView kWhite{0x1002};

DrawList fresh_list() {
    DrawList dl;
    dl.set_default_texture(kWhite);
    return dl;
}

struct Bounds {
    glm::vec2 min{1e30f}, max{-1e30f};
    size_t    count = 0;
    bool      any() const { return count > 0; }
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
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.u) || !std::isfinite(v.v)) return false;
    }
    return true;
}

/** @brief The colour of the first vertex emitted -- the background quad is always first. */
uint32_t background_color(const DrawList& dl) {
    return dl.vertices().empty() ? 0u : dl.vertices()[0].color;
}

}  // namespace

COOPA_TEST(background_covers_the_window_behind_the_mark) {
    const glm::vec2 canvas(1600.0f, 900.0f);
    toy::core::BootSplashStyle style;
    DrawList dl = fresh_list();
    toy::core::emit_boot_splash(dl, canvas, style, 0.0f);

    expect(!dl.indices().empty(), "splash: something was emitted");
    expect(all_finite(dl), "splash: every vertex is finite");

    const Bounds all = bounds_of(dl);
    expect(all.min.x <= 0.0f && all.min.y <= 0.0f && all.max.x >= canvas.x && all.max.y >= canvas.y,
           "splash: the background spans the whole window");
}

COOPA_TEST(the_mark_draws_on_top_of_the_background) {
    const glm::vec2 canvas(1280.0f, 720.0f);
    toy::core::BootSplashStyle style;
    DrawList dl = fresh_list();
    toy::core::emit_boot_splash(dl, canvas, style, 0.0f);

    // The background quad is 4 vertices / 6 indices; the mark (emit_loading_screen) adds its
    // own 3-faces-of-2-triangles on top when style.loading.spinner (the default) is true.
    expect(dl.vertices().size() > 4, "splash: the mark drew vertices beyond the background quad");
    expect(dl.indices().size() > 6, "splash: the mark drew indices beyond the background quad");
}

COOPA_TEST(spinner_off_leaves_only_the_background) {
    const glm::vec2 canvas(1280.0f, 720.0f);
    toy::core::BootSplashStyle style;
    style.loading.spinner = false;
    DrawList dl = fresh_list();
    toy::core::emit_boot_splash(dl, canvas, style, 0.0f);

    expect(dl.vertices().size() == 4 && dl.indices().size() == 6,
           "splash: loading.spinner = false draws only the background quad");
}

COOPA_TEST(linearize_decodes_the_background_colour_before_packing) {
    const glm::vec2 canvas(1280.0f, 720.0f);
    toy::core::BootSplashStyle style;
    style.loading.background = glm::vec3(0.5f, 0.5f, 0.5f);

    style.linearize = false;
    DrawList raw = fresh_list();
    toy::core::emit_boot_splash(raw, canvas, style, 0.0f);

    style.linearize = true;
    DrawList linear = fresh_list();
    toy::core::emit_boot_splash(linear, canvas, style, 0.0f);

    expect(background_color(raw) != background_color(linear),
           "linearize: an sRGB attachment packs a different (pre-decoded) colour than a linear one");
}

COOPA_TEST(the_layout_survives_odd_canvases) {
    toy::core::BootSplashStyle style;
    for (glm::vec2 canvas : {glm::vec2(2560.0f, 1280.0f), glm::vec2(600.0f, 1200.0f),
                             glm::vec2(320.0f, 200.0f), glm::vec2(3840.0f, 2160.0f)}) {
        DrawList dl = fresh_list();
        toy::core::emit_boot_splash(dl, canvas, style, 1.5f);
        const std::string where = std::to_string(static_cast<int>(canvas.x)) + "x" +
                                  std::to_string(static_cast<int>(canvas.y));
        expect(all_finite(dl), "aspect " + where + ": every vertex is finite");
        const Bounds all = bounds_of(dl);
        expect(all.min.x >= 0.0f && all.max.x <= canvas.x && all.min.y >= 0.0f && all.max.y <= canvas.y,
               "aspect " + where + ": everything stays on screen");
    }

    // A zero-sized window (minimized) emits nothing rather than dividing by it.
    DrawList none = fresh_list();
    toy::core::emit_boot_splash(none, glm::vec2(0.0f, 0.0f), toy::core::BootSplashStyle{}, 0.0f);
    expect(none.indices().empty(), "a zero-sized window emits no geometry");
}

COOPA_TEST(the_mark_animates_over_time_like_a_scene_loads_own_cover) {
    const glm::vec2 canvas(1280.0f, 720.0f);
    toy::core::BootSplashStyle style;
    auto colors_at = [&](float time) {
        DrawList dl = fresh_list();
        toy::core::emit_boot_splash(dl, canvas, style, time);
        std::vector<uint32_t> cols;
        for (const coopa::ui::UiVertex& v : dl.vertices()) cols.push_back(v.color);
        return cols;
    };

    const std::vector<uint32_t> at0 = colors_at(0.0f);
    const std::vector<uint32_t> atMid = colors_at(0.4f);
    expect(at0 != atMid, "splash: the mark's cycle changes the frame over time");
}
