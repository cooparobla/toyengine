/**
 * @file text_atlas_registry.h
 * @brief Process-wide record of every R8 glyph-atlas TextureView that exists.
 *
 * A UI pass has to know which of its bound textures are R8 coverage atlases rather than RGBA
 * sprites, so it can select the "text" pipeline variant for them -- get it wrong and a glyph
 * atlas's coverage values are drawn as colour, i.e. solid blocks instead of letters (see
 * UiPass::is_text_view_).
 *
 * A list built at scene-parse time from authored `font_size`s can't supply it: Text bakes at
 * `font_size * the canvas's effective text scale`, so the baked size is a runtime property of
 * the window and the canvas.
 *
 * So registration happens in the one place that cannot be out of date: FontAtlas's
 * constructor. Every atlas that exists is in here, whoever created it and whenever; the
 * destructor removes it again. Hosts call UIResourceCache::mark_text_atlases() per frame.
 *
 * Not thread-safe, and deliberately so -- atlas creation happens on the thread that emits UI,
 * which is the same thread that records draw commands.
 */

#ifndef UICOOPA_TEXT_TEXT_ATLAS_REGISTRY_H
#define UICOOPA_TEXT_TEXT_ATLAS_REGISTRY_H

#include <gfxcoopa/types/texture_view.h>

#include <algorithm>
#include <vector>

namespace coopa {
namespace ui {

/**
 * @class TextAtlasRegistry
 * @brief Singleton list of live glyph-atlas texture views; see the file doc.
 */
class TextAtlasRegistry {
public:
    /** @brief The process-wide instance. */
    static TextAtlasRegistry& instance() {
        static TextAtlasRegistry registry;
        return registry;
    }

    /** @brief Records a view as an R8 glyph atlas. Called by FontAtlas's constructor. */
    void add(coopa::gfx::TextureView view) {
        if (view == coopa::gfx::TextureView{}) return;
        if (std::find(views_.begin(), views_.end(), view) == views_.end()) views_.push_back(view);
    }

    /**
     * @brief Forgets a view. Called by FontAtlas's destructor.
     *
     * Leaving a destroyed atlas's view in here would be worse than a leak: views are identity
     * tokens, and a later texture could be handed the same underlying handle and then be
     * mis-drawn through the "text" variant.
     */
    void remove(coopa::gfx::TextureView view) {
        views_.erase(std::remove(views_.begin(), views_.end(), view), views_.end());
    }

    /** @brief Drops every entry; see UIResourceCache::clear(), which destroys every Font. */
    void clear() { views_.clear(); }

    /**
     * @brief Marks every known glyph atlas on a pass.
     *
     * @tparam Pass Anything with `mark_as_text_atlas(coopa::gfx::TextureView)` -- both UiPass
     *   and UiWorldPass qualify, and a canvas drawn by both must be registered with both.
     */
    template <typename Pass>
    void mark_all(Pass& pass) const {
        for (coopa::gfx::TextureView view : views_) pass.mark_as_text_atlas(view);
    }

    /** @brief The live views, for diagnostics. */
    const std::vector<coopa::gfx::TextureView>& views() const { return views_; }

private:
    TextAtlasRegistry() = default;

    // A vector, not an unordered_set: this holds one entry per (font, size, oversample) actually
    // baked -- single digits in practice -- and mark_all() iterates it every frame, where
    // contiguous storage beats hashing.
    std::vector<coopa::gfx::TextureView> views_;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_TEXT_TEXT_ATLAS_REGISTRY_H
