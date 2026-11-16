/**
 * @file glass_theme.h
 * @brief A glassmorphism screen's own colour roles, as a `glass:` section of an `imm::Theme`.
 *
 * Mirrors game_theme.h's shape exactly: `GameTheme` carries the roles every game screen might
 * want (bars, slots, feedback, world plates); this carries the roles specific to one LOOK --
 * the frosted-glass backdrop gradient, its soft colour blobs, and the glass pane itself. A
 * screen reads it through `on_theme_loaded()` (`ImmScreen`'s hook for a section `GameTheme`
 * does not cover), the same way any screen reads `theme()` for the roles that already are.
 *
 * @code{.yaml}
 * # a theme's glass: section, beside colors: / bars: / etc.
 * glass:
 *   fill:        "#ffffff26"   # rgba(255,255,255,0.15)
 *   border:      "#ffffff33"   # rgba(255,255,255,0.2)
 *   grad_blue:   "#0080ff"
 *   grad_violet: "#8b00ff"
 *   grad_pink:   "#ff1493"
 *   blob_teal:   "#20b2aa"
 *   blob_pink:   "#ff1493"
 *   text_shadow: "#00000040"   # 0 1px 2px rgba(0,0,0,0.25)
 *   opaque_fill: "#1e143cd9"   # prefers-reduced-transparency fallback
 * @endcode
 */

#ifndef TOYENGINE_UI_GLASS_THEME_H
#define TOYENGINE_UI_GLASS_THEME_H

#include <glm/glm.hpp>

// Declared, not included -- see game_theme.h's identical comment: imm_theme.h is what pulls in
// fkYAML, and only the loader (glass_theme.cpp) needs it.
namespace coopa::ui::imm { struct Theme; }

namespace toy::ui {

/**
 * @struct GlassTheme
 * @brief The colour roles one glassmorphism screen needs, beyond the widget Style and beyond
 *        GameTheme's fixed bars/slots/feedback/world roles.
 *
 * Deliberately specific to the look rather than general-purpose: a second, differently-styled
 * screen would define its own section rather than grow this one.
 */
struct GlassTheme {
    /** @brief The frosted pane itself -- Context::glass_panel()'s default tint/rim colours. */
    glm::vec4 fill  {1.0f, 1.0f, 1.0f, 0.15f};
    glm::vec4 border{1.0f, 1.0f, 1.0f, 0.20f};

    /** @brief The three-stop 135-degree backdrop gradient behind the glass. */
    glm::vec4 grad_blue  {0.000f, 0.502f, 1.000f, 1.0f};
    glm::vec4 grad_violet{0.545f, 0.000f, 1.000f, 1.0f};
    glm::vec4 grad_pink  {1.000f, 0.078f, 0.576f, 1.0f};

    /** @brief Soft colour blobs drawn over the gradient, under the glass. */
    glm::vec4 blob_teal{0.125f, 0.698f, 0.667f, 1.0f};
    glm::vec4 blob_pink{1.000f, 0.078f, 0.576f, 1.0f};

    /** @brief Drawn behind text over the backdrop, so it reads against any hue the glass shows. */
    glm::vec4 text_shadow{0.0f, 0.0f, 0.0f, 0.25f};

    /** @brief Opaque panel fill a reduced-transparency mode swaps the frosted pane for. */
    glm::vec4 opaque_fill{0.118f, 0.078f, 0.235f, 0.851f};
};

/**
 * @brief Calls `v(section, role, colour&)` for every glass role -- the single list of them,
 *        walked by the loader and by any editor that offers these as rows.
 */
template <class T, class V>
void visit_glass_theme(T& t, V&& v) {
    v("glass", "fill", t.fill);
    v("glass", "border", t.border);
    v("glass", "grad_blue", t.grad_blue);
    v("glass", "grad_violet", t.grad_violet);
    v("glass", "grad_pink", t.grad_pink);
    v("glass", "blob_teal", t.blob_teal);
    v("glass", "blob_pink", t.blob_pink);
    v("glass", "text_shadow", t.text_shadow);
    v("glass", "opaque_fill", t.opaque_fill);
}

/** @brief The glass roles `theme` defines; any it omits keeps GlassTheme's own value. */
GlassTheme glass_theme_from(const coopa::ui::imm::Theme& theme);

}  // namespace toy::ui

#endif  // TOYENGINE_UI_GLASS_THEME_H
