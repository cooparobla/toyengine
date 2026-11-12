/**
 * @file icon_library.h
 * @brief Process-wide registry merging any number of loaded SpriteSheets into one
 *        name -> Sprite lookup, backed by coopa::asset::AssetManager.
 *
 * A SpriteSheet's own Sprite table (sprite_sheet.h) is owned by the AssetHandle's
 * const payload, so nothing outside the AssetManager may mutate it -- and
 * AssetHandle<T>::get() only ever returns `const T*` (see asset_handle.h). IconLibrary
 * therefore does NOT hand out pointers into a SpriteSheet's own map: for every published
 * name it owns a separate, mutable Sprite copy. On a hot reload (AssetManager::on_reloaded)
 * that copy is rewritten in place with the new texture/uv/border rather than replaced --
 * so an Image::sprite (or an InventorySlot's drag-ghost copy, or anything else already
 * pointing at a published icon) keeps working across the reload instead of dangling.
 *
 * ui_yaml.h's UIResourceCache::sprite_for() consults this library (by name) between its
 * existing UIResources check and its path-based texture load, so scene YAML's existing
 * `sprite: <name>` lookup resolves an icon with zero parser changes.
 */

#ifndef UICOOPA_RENDER_ICON_LIBRARY_H
#define UICOOPA_RENDER_ICON_LIBRARY_H

#include <coopa/asset/asset_handle.h>
#include <coopa/asset/asset_manager.h>
#include <coopa/event/signal.h>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/command/command_pool.h>

#include <uicoopa/render/sprite.h>
#include <uicoopa/render/sprite_sheet.h>
#include <uicoopa/render/sprite_sheet_loader.h>

#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace coopa {
namespace ui {

/**
 * @class IconLibrary
 * @brief Loads sprite sheets through a coopa::asset::AssetManager and publishes their
 *        entries as pointer-stable, mutable Sprites.
 *
 * Non-owning of the AssetManager/Device/Allocator/CommandPool it's configured with --
 * the application must keep them alive for as long as IconLibrary is used, and must call
 * clear() before they're destroyed (mirrors UIResourceCache::clear()'s contract exactly).
 */
class IconLibrary {
public:
    static IconLibrary& instance() {
        static IconLibrary library;
        return library;
    }

    /**
     * @brief Registers SpriteSheetLoader on `assets`. Call once at startup, before any
     *        add_sheet() call that uses this manager.
     */
    void configure(coopa::asset::AssetManager& assets,
                  coopa::gfx::core::Device& device,
                  coopa::gfx::memory::Allocator& allocator,
                  coopa::gfx::command::CommandPool& cmd_pool) {
        assets.register_loader<SpriteSheet>(std::make_unique<SpriteSheetLoader>(device, allocator, cmd_pool));
    }

    /**
     * @brief Loads a sheet synchronously through `assets` and publishes its entries.
     *
     * Each entry is registered under its bare name (e.g. "arrow_left") and, if `prefix`
     * is non-empty, also under "<prefix>arrow_left" -- so a second sheet can supply its
     * own "arrow_left" under a distinct prefix without permanently losing the first
     * sheet's entry, even though the bare name is last-write-wins.
     *
     * @param assets Manager to load through; must have had configure() called on it.
     * @param virtual_path Descriptor (.yaml) or bare image path, e.g. "icons/icons.yaml".
     * @param prefix Optional name prefix, e.g. "game/" -> "game/arrow_left".
     * @return True if the sheet loaded; false (with a stderr message) leaves the
     *         registry untouched.
     */
    bool add_sheet(coopa::asset::AssetManager& assets, const std::string& virtual_path,
                  const std::string& prefix = "") {
        coopa::asset::AssetHandle<SpriteSheet> handle = assets.load<SpriteSheet>(virtual_path);
        if (!handle.is_loaded()) {
            std::cerr << "[uicoopa] IconLibrary: failed to load sheet '" << virtual_path
                      << "': " << handle.error() << "\n";
            return false;
        }

        republish_(*handle, prefix);

        coopa::event::Connection conn = assets.on_reloaded.connect(
            [this, prefix, handle](const coopa::asset::AssetId& id) {
                if (id == handle.id() && handle.is_loaded()) republish_(*handle, prefix);
            });

        sheets_.push_back(LoadedSheet{handle, std::move(conn)});
        return true;
    }

    /** @brief Looks up a published icon. @return Pointer-stable Sprite, or nullptr if unpublished. */
    Sprite* icon(const std::string& name) const {
        auto it = sprites_.find(name);
        return it != sprites_.end() ? it->second.get() : nullptr;
    }

    /** @brief True once at least one icon has been published. */
    bool has_icons() const { return !sprites_.empty(); }

    /**
     * @brief Drops every AssetHandle, disconnects every hot-reload subscription, and frees
     *        every owned Sprite.
     *
     * This is a process-wide singleton holding GPU-resident payloads (via its
     * AssetHandles) and Sprite copies -- call this before the AssetManager it was
     * configured with is shut down and before its Device/Allocator are destroyed,
     * mirroring UIResourceCache::clear()'s contract.
     */
    void clear() {
        for (auto& s : sheets_) s.reload_connection.disconnect();
        sheets_.clear();
        sprites_.clear();
    }

private:
    struct LoadedSheet {
        coopa::asset::AssetHandle<SpriteSheet> handle;
        coopa::event::Connection               reload_connection;
    };

    /** @brief (Re)writes every sprite of `sheet` into sprites_, in place if already present. */
    void republish_(const SpriteSheet& sheet, const std::string& prefix) {
        for (const auto& [name, sprite] : sheet.sprites()) {
            assign_(name, sprite);
            if (!prefix.empty()) assign_(prefix + name, sprite);
        }
    }

    void assign_(const std::string& name, const Sprite& value) {
        auto it = sprites_.find(name);
        if (it == sprites_.end()) {
            sprites_.emplace(name, std::make_unique<Sprite>(value));
        } else {
            *it->second = value; // rewrite in place -- keeps every outstanding Sprite* valid
        }
    }

    std::vector<LoadedSheet>                             sheets_;
    std::unordered_map<std::string, std::unique_ptr<Sprite>> sprites_;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_RENDER_ICON_LIBRARY_H
