/**
 * @file ui_resources.h
 * @brief The process-wide font cache the UI loads through, and the text-atlas marking a host
 *        needs before it draws.
 *
 * Two things every UI host needs and nothing else owns:
 *
 *  * **Fonts by path.** Loaded once and shared, so the same face referenced from a theme, a
 *    screen and a world plate costs one atlas rather than three. The first one loaded becomes
 *    FontDefaults::font, which is what an unconfigured widget falls back to.
 *  * **Text atlases.** Glyph atlases are R8 coverage images and have to be drawn through the
 *    text pipeline variant rather than the quad one. A host marks them on its UiPass each
 *    frame before recording; TextAtlasRegistry knows every atlas that has ever been baked, so
 *    this stays correct for atlases created lazily mid-frame.
 */

#ifndef UICOOPA_UI_RESOURCES_H
#define UICOOPA_UI_RESOURCES_H

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>

#include <uicoopa/text/font.h>
#include <uicoopa/text/font_defaults.h>
#include <uicoopa/text/text_atlas_registry.h>

namespace coopa {
namespace ui {

/**
 * @class UIResourceCache
 * @brief Owns GPU-resident Fonts loaded on demand, keyed by resolved path.
 */
class UIResourceCache {
public:
    static UIResourceCache& instance() {
        static UIResourceCache cache;
        return cache;
    }

    /** @brief Supplies the GPU handles fonts are created with. Call once at startup. */
    void configure(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                   coopa::gfx::command::CommandPool& cmd_pool) {
        device_ = &device;
        allocator_ = &allocator;
        cmd_pool_ = &cmd_pool;
    }

    /**
     * @brief The Font for a resolved filesystem path, loading it once.
     * @return Null with no GPU set (headless), or if the file will not load.
     */
    Font* font_for_path(const std::string& resolved) {
        if (!device_ || resolved.empty()) return nullptr;
        auto it = fonts_by_path_.find(resolved);
        if (it != fonts_by_path_.end()) return it->second.get();
        try {
            auto font = std::make_unique<Font>(*device_, *allocator_, *cmd_pool_, resolved);
            Font* raw = font.get();
            fonts_by_path_.emplace(resolved, std::move(font));
            // The first face loaded becomes the fallback, so a widget with no font of its own
            // still draws rather than silently going blank.
            if (!FontDefaults::font) FontDefaults::font = raw;
            return raw;
        } catch (const std::exception& e) {
            std::cerr << "[uicoopa] Failed to load font '" << resolved << "': " << e.what() << "\n";
            return nullptr;
        }
    }

    /** @brief Any loaded font, or null. */
    Font* default_font() const {
        return fonts_by_path_.empty() ? nullptr : fonts_by_path_.begin()->second.get();
    }

    /**
     * @brief Tells `ui_pass` which of its textures are glyph atlases.
     *
     * Call once per frame before recording. Atlases are R8 coverage, not colour, so a pass
     * that drew one through the quad pipeline would render solid blocks.
     */
    template <typename Pass>
    void mark_text_atlases(Pass& ui_pass) {
        TextAtlasRegistry::instance().mark_all(ui_pass);
    }

    /** @brief Drops every cached font. Call before the device goes away. */
    void clear() {
        FontDefaults::font = nullptr;
        fonts_by_path_.clear();
    }

private:
    coopa::gfx::core::Device* device_ = nullptr;
    coopa::gfx::memory::Allocator* allocator_ = nullptr;
    coopa::gfx::command::CommandPool* cmd_pool_ = nullptr;
    std::unordered_map<std::string, std::unique_ptr<Font>> fonts_by_path_;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_UI_RESOURCES_H
