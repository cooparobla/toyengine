/**
 * @file thumbnail_cache.h
 * @brief Small GPU copies of image assets, so the editor's UI can show a texture instead of
 *        its file name.
 *
 * Each entry is one image shrunk to at most 128 pixels on its longest side (see
 * editor/ui/image_downsample.h for why shrinking is not optional) and uploaded as RGBA8.
 *
 * Two rules come from the UI pass that draws these:
 *
 *  - It registers every texture it sees in a descriptor cache keyed by the raw image view,
 *    and that cache never evicts on its own. Destroying a texture without telling the pass
 *    first lets the driver hand the same handle to the next view created, which the pass
 *    would then skip registering -- silently drawing the dead image. So eviction here is
 *    always: wait for the device to go idle, unregister, then destroy.
 *  - Its descriptor pool is finite (256 sets), so the cache is capped well under that and
 *    drops the least recently drawn entry when full.
 *
 * Loading is synchronous but budgeted: a few images per frame, so opening a folder of
 * textures costs a few frames of filling in rather than one long stall. A thumbnail that is
 * not ready yet is simply not drawn -- callers fall back to the file name.
 */

#ifndef TOYEDITOR_APP_THUMBNAIL_CACHE_H
#define TOYEDITOR_APP_THUMBNAIL_CACHE_H

#include <gfxcoopa/command/command_pool.h>
#include <gfxcoopa/core/device.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/memory/allocator.h>
#include <gfxcoopa/types/texture_view.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace toy::editor {

namespace fs = std::filesystem;

class ThumbnailCache {
public:
    /** @brief The longest side of a thumbnail, in pixels. */
    static constexpr int kSize = 128;
    /** @brief How many entries may be resident; the UI pass's pool holds 256 descriptor sets. */
    static constexpr size_t kMaxEntries = 128;
    /** @brief Images decoded and uploaded per frame, so a big folder fills in over a few. */
    static constexpr int kLoadsPerFrame = 2;

    /**
     * @param unregister Called with a view just before its texture is destroyed; the host
     *                   passes ToyRenderPipeline::unregister_screen_ui_texture. Must also have
     *                   waited for the device to be idle -- see the file doc.
     */
    ThumbnailCache(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                   coopa::gfx::command::CommandPool& cmd_pool,
                   std::function<void(coopa::gfx::TextureView)> unregister);
    ~ThumbnailCache();

    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    /** @brief Resets the per-frame load budget and advances the clock LRU uses. Call once a frame. */
    void begin_frame();

    /**
     * @brief The thumbnail for the image file at `path`.
     * @return Its view, or a null view while it has not been loaded yet (or could not be).
     */
    coopa::gfx::TextureView view(const fs::path& path);

    /** @brief Forgets `path`, so the next view() reloads it (the file changed on disk). */
    void invalidate(const fs::path& path);

    /** @brief Forgets everything -- a different project is open. */
    void clear();

    /** @brief How many thumbnails are resident, for the status bar and tests. */
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::unique_ptr<coopa::gfx::engine::data::Texture> texture;
        bool failed = false;     ///< Decoding failed: remembered so it is not retried every frame.
        uint64_t last_used = 0;
    };

    bool load_(const std::string& key, Entry& entry);
    void evict_oldest_();
    void destroy_(Entry& entry);

    coopa::gfx::core::Device& device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::gfx::command::CommandPool& cmd_pool_;
    std::function<void(coopa::gfx::TextureView)> unregister_;
    std::map<std::string, Entry> entries_;
    uint64_t clock_ = 0;
    int budget_ = kLoadsPerFrame;
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_THUMBNAIL_CACHE_H
