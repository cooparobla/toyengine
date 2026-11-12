#include "editor/app/thumbnail_cache.h"

#include "editor/ui/image_downsample.h"

#include <gfxcoopa/types/format.h>

#include <stb/stb_image.h>

#include <algorithm>

namespace toy {
namespace editor {

ThumbnailCache::ThumbnailCache(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                               coopa::gfx::command::CommandPool& cmd_pool,
                               std::function<void(coopa::gfx::TextureView)> unregister)
    : device_(device), allocator_(allocator), cmd_pool_(cmd_pool), unregister_(std::move(unregister)) {}

ThumbnailCache::~ThumbnailCache() { clear(); }

void ThumbnailCache::begin_frame() {
    ++clock_;
    budget_ = kLoadsPerFrame;
}

coopa::gfx::TextureView ThumbnailCache::view(const fs::path& path) {
    if (path.empty()) return {};
    const std::string key = path.string();
    auto it = entries_.find(key);
    if (it == entries_.end()) {
        if (budget_ <= 0) return {};   // next frame; the caller draws its fallback meanwhile
        --budget_;
        if (entries_.size() >= kMaxEntries) evict_oldest_();
        it = entries_.emplace(key, Entry{}).first;
        if (!load_(key, it->second)) it->second.failed = true;
    }
    it->second.last_used = clock_;
    return it->second.texture ? it->second.texture->view_typed() : coopa::gfx::TextureView{};
}

void ThumbnailCache::invalidate(const fs::path& path) {
    auto it = entries_.find(path.string());
    if (it == entries_.end()) return;
    destroy_(it->second);
    entries_.erase(it);
}

void ThumbnailCache::clear() {
    for (auto& [key, entry] : entries_) destroy_(entry);
    entries_.clear();
}

bool ThumbnailCache::load_(const std::string& key, Entry& entry) {
    int w = 0, h = 0, channels = 0;
    stbi_uc* pixels = stbi_load(key.c_str(), &w, &h, &channels, STBI_rgb_alpha);
    if (!pixels) return false;
    const ui::Rgba8Image small = ui::downsample_rgba8(pixels, w, h, kSize);
    stbi_image_free(pixels);
    if (small.empty()) return false;
    try {
        entry.texture = std::make_unique<coopa::gfx::engine::data::Texture>(
            coopa::gfx::engine::data::Texture::upload(device_, allocator_, cmd_pool_, small.pixels.data(),
                                                      static_cast<uint32_t>(small.width),
                                                      static_cast<uint32_t>(small.height),
                                                      coopa::gfx::Format::RGBA8_Unorm));
    } catch (const std::exception&) {
        entry.texture.reset();
        return false;
    }
    return true;
}

void ThumbnailCache::evict_oldest_() {
    auto oldest = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->second.last_used == clock_) continue;   // drawn this frame: not a candidate
        if (oldest == entries_.end() || it->second.last_used < oldest->second.last_used) oldest = it;
    }
    if (oldest == entries_.end()) return;
    destroy_(oldest->second);
    entries_.erase(oldest);
}

void ThumbnailCache::destroy_(Entry& entry) {
    if (!entry.texture) return;
    // The UI pass holds a descriptor set keyed by this view's handle. Let the GPU finish with
    // it, drop the pass's entry, and only then free the image -- otherwise a later texture can
    // be handed the same handle and inherit a descriptor pointing at freed memory.
    device_.wait_idle();
    if (unregister_) unregister_(entry.texture->view_typed());
    entry.texture.reset();
}

} // namespace editor
} // namespace toy
