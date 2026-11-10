/**
 * @file font_defaults.h
 * @brief Process-wide fallback font used by widgets that need one but weren't
 *        handed a specific Font (e.g. auto-generated tooltip/inventory text).
 *
 * Application code sets FontDefaults::font once at startup (typically right
 * after loading its UI font -- see ui_yaml.h's UIResources::register_font()
 * and UIResourceCache::font_for(), which both do this automatically) and
 * FontDefaults::note_text_size so every (font, pixel size) pair actually used
 * gets reported back to whatever owns atlas baking (see
 * UIResourceCache::mark_text_atlases()).
 */

#ifndef UICOOPA_TEXT_FONT_DEFAULTS_H
#define UICOOPA_TEXT_FONT_DEFAULTS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace coopa {
namespace ui {

/**
 * @struct FontDefaults
 * @brief Process-wide fallback font + text-atlas-use hook.
 *
 * Both members are non-owning: the application must keep the referenced Font
 * alive for as long as any widget might fall back to it.
 */
struct FontDefaults {
    /** @brief Fallback font used when a widget or theme has none of its own. */
    static inline class Font* font = nullptr;

    /** @brief Called with every (font, pixel size) pair actually assigned to a Text, so the
     *         application can bake exactly the glyph atlases it needs. */
    static inline std::function<void(class Font*, uint32_t)> note_text_size = nullptr;

    /**
     * @brief Loads (or returns an already-cached) Font for a filesystem path, so
     *        ui_theme_yaml.h's resolve_theme_fonts() can populate UITheme::font
     *        and each typography role's FontRoleStyle::font without this header
     *        (or ui_theme_yaml.h) depending on the GPU-backed loader that
     *        actually owns Font instances -- see ui_yaml.h's UIResourceCache::
     *        font_for_path(), which applications typically bind this to. Null in
     *        headless builds, so an unresolved path just leaves the theme's
     *        Font* fields null and every role falls back to FontDefaults::font.
     */
    static inline std::function<class Font*(const std::string&)> resolve_font = nullptr;

    /**
     * @brief Extra directories a theme's font paths are looked up in, highest priority first.
     *
     * A theme states its fonts relative to itself ("../../fonts/inter_regular.ttf"), which is
     * right for a theme shipped beside the fonts it uses -- and wrong for one in a game's own
     * assets that wants the engine's shared faces. An application that layers asset
     * directories sets this to those roots at startup (the font then reads
     * "fonts/inter_regular.ttf", resolved against each in turn), and
     * ui_theme_yaml.h's resolve_theme_fonts() tries them after the theme's own folder.
     */
    static inline std::vector<std::string> search_dirs{};
};


}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_TEXT_FONT_DEFAULTS_H
