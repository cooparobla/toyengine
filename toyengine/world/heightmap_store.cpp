#include <toyengine/world/heightmap_store.h>

#include <algorithm>
#include <cmath>
#include <iostream>

#include <toyengine/world/png16.h>

namespace toy {
namespace world {

namespace {

/** @brief Bilinear resample of a 16-bit grid to (dw, dh), point-sampled corner to corner. */
std::vector<float> resample_u16(const std::vector<std::uint16_t>& src, int sw, int sh, int dw, int dh, float scale) {
    std::vector<float> out(static_cast<std::size_t>(dw) * dh);
    for (int j = 0; j < dh; ++j) {
        const float v = dh > 1 ? static_cast<float>(j) * static_cast<float>(sh - 1) / static_cast<float>(dh - 1) : 0.0f;
        const int j0 = std::min(static_cast<int>(v), sh - 1), j1 = std::min(j0 + 1, sh - 1);
        const float fy = v - static_cast<float>(j0);
        for (int i = 0; i < dw; ++i) {
            const float u = dw > 1 ? static_cast<float>(i) * static_cast<float>(sw - 1) / static_cast<float>(dw - 1) : 0.0f;
            const int i0 = std::min(static_cast<int>(u), sw - 1), i1 = std::min(i0 + 1, sw - 1);
            const float fx = u - static_cast<float>(i0);
            auto s = [&](int x, int y) { return static_cast<float>(src[static_cast<std::size_t>(y) * sw + x]); };
            const float a = s(i0, j0) + (s(i1, j0) - s(i0, j0)) * fx;
            const float b = s(i0, j1) + (s(i1, j1) - s(i0, j1)) * fx;
            out[static_cast<std::size_t>(j) * dw + i] = (a + (b - a) * fy) * scale;
        }
    }
    return out;
}

} // namespace

bool load_heightmap_image(HeightmapData& data, const std::string& path) {
    int w = 0, h = 0;
    std::vector<std::uint16_t> px;
    if (!read_png16_grey(path, w, h, px)) return false;
    if (w < 2 || h < 2) return false;
    // PNG rows run top-down; the grid's j axis runs +y (north). Flip so an image viewed in any
    // paint program shows north up, the way a map reads.
    std::vector<std::uint16_t> flipped(px.size());
    for (int y = 0; y < h; ++y) {
        std::copy_n(px.begin() + static_cast<std::ptrdiff_t>(y) * w, w,
                    flipped.begin() + static_cast<std::ptrdiff_t>(h - 1 - y) * w);
    }
    data.heights() = resample_u16(flipped, w, h, data.nx(), data.ny(), data.geometry().height / 65535.0f);
    data.mark_all_dirty();
    return true;
}

bool save_heightmap_image(const HeightmapData& data, const std::string& path) {
    const int w = data.nx(), h = data.ny();
    std::vector<std::uint16_t> px(static_cast<std::size_t>(w) * h);
    const float k = 65535.0f / data.geometry().height;
    for (int j = 0; j < h; ++j) {
        for (int i = 0; i < w; ++i) {
            const float v = std::clamp(data.at(i, j) * k + 0.5f, 0.0f, 65535.0f);
            px[static_cast<std::size_t>(h - 1 - j) * w + i] = static_cast<std::uint16_t>(v);
        }
    }
    return write_png16_grey(path, w, h, px);
}

bool load_splat_image(HeightmapData& data, int index, const std::string& path) {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
    if (!read_png_rgba8(path, w, h, px)) return false;
    const int dw = data.splat_w(), dh = data.splat_h();
    std::vector<std::uint8_t>& dst = data.splat(index);
    for (int j = 0; j < dh; ++j) {
        // Nearest texel-centred sample, flipped to match the heightmap convention.
        const int sy = std::clamp(static_cast<int>((static_cast<float>(j) + 0.5f) * static_cast<float>(h) / static_cast<float>(dh)), 0, h - 1);
        const int row = h - 1 - sy;
        for (int i = 0; i < dw; ++i) {
            const int sx = std::clamp(static_cast<int>((static_cast<float>(i) + 0.5f) * static_cast<float>(w) / static_cast<float>(dw)), 0, w - 1);
            for (int c = 0; c < 4; ++c) {
                dst[(static_cast<std::size_t>(j) * dw + i) * 4 + c] = px[(static_cast<std::size_t>(row) * w + sx) * 4 + c];
            }
        }
    }
    data.mark_splat_dirty(IRect{0, 0, dw, dh});
    return true;
}

bool save_splat_image(const HeightmapData& data, int index, const std::string& path) {
    const int w = data.splat_w(), h = data.splat_h();
    const std::vector<std::uint8_t>& src = data.splat(index);
    std::vector<std::uint8_t> px(src.size());
    for (int j = 0; j < h; ++j) {
        std::copy_n(src.begin() + static_cast<std::ptrdiff_t>(j) * w * 4, w * 4,
                    px.begin() + static_cast<std::ptrdiff_t>(h - 1 - j) * w * 4);
    }
    return write_png_rgba8(path, w, h, px);
}

HeightmapStore& HeightmapStore::instance() {
    static HeightmapStore store;
    return store;
}

std::filesystem::file_time_type HeightmapStore::mtime_(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

void HeightmapStore::load_entry_(Entry& entry, const HeightmapGeometry& geometry, const HeightmapGenerate& generate) {
    entry.data->reset(geometry);
    if (!load_heightmap_image(*entry.data, entry.files.height)) {
        entry.data->generate(generate);
    }
    // Splats are optional: a missing pair leaves layer 0 everywhere (reset()'s fill).
    bool any_splat = false;
    if (!entry.files.splat0.empty()) any_splat |= load_splat_image(*entry.data, 0, entry.files.splat0);
    if (!entry.files.splat1.empty()) any_splat |= load_splat_image(*entry.data, 1, entry.files.splat1);
    if (any_splat) {
        for (int j = 0; j < entry.data->splat_h(); ++j) {
            for (int i = 0; i < entry.data->splat_w(); ++i) entry.data->normalize_splat_texel(i, j);
        }
    }
    entry.data->mark_all_dirty();
    entry.data->set_saved();
    entry.height_mtime = mtime_(entry.files.height);
}

std::shared_ptr<HeightmapData> HeightmapStore::acquire(const HeightmapFiles& files, const HeightmapGeometry& geometry_in,
                                                       const HeightmapGenerate& generate) {
    const HeightmapGeometry geometry = geometry_in.sanitized();
    auto it = entries_.find(files.height);
    if (it == entries_.end()) {
        Entry entry;
        entry.files = files;
        entry.data = std::make_shared<HeightmapData>(geometry);
        load_entry_(entry, geometry, generate);
        it = entries_.emplace(files.height, std::move(entry)).first;
        return it->second.data;
    }

    Entry& entry = it->second;
    entry.files = files;
    HeightmapData& data = *entry.data;

    // Edited elsewhere (another tool, or the editor process saving while a game runs).
    if (!data.dirty_on_disk() && mtime_(files.height) != entry.height_mtime) {
        entry.shapes.clear();
        load_entry_(entry, geometry, generate);
        return entry.data;
    }

    if (data.geometry() != geometry) {
        // Remember the current shape, then restore the requested one exactly if we have it.
        auto cached = std::find_if(entry.shapes.begin(), entry.shapes.end(),
                                   [&](const HeightmapData& d) { return d.geometry() == geometry; });
        HeightmapData previous = data;
        if (cached != entry.shapes.end()) {
            data.assign_content(*cached);
            entry.shapes.erase(cached);
        } else {
            data.set_geometry(geometry);
        }
        entry.shapes.push_back(std::move(previous));
        if (entry.shapes.size() > 4) entry.shapes.erase(entry.shapes.begin());
    }
    return entry.data;
}

HeightmapFiles HeightmapStore::files_of(const HeightmapData* data) const {
    for (const auto& [key, entry] : entries_) {
        if (entry.data.get() == data) return entry.files;
    }
    return {};
}

bool HeightmapStore::save(const HeightmapData* data) {
    for (auto& [key, entry] : entries_) {
        if (entry.data.get() != data) continue;
        bool ok = save_heightmap_image(*entry.data, entry.files.height);
        if (!entry.files.splat0.empty()) ok &= save_splat_image(*entry.data, 0, entry.files.splat0);
        if (!entry.files.splat1.empty()) ok &= save_splat_image(*entry.data, 1, entry.files.splat1);
        if (ok) {
            entry.data->set_saved();
            entry.height_mtime = mtime_(entry.files.height);
        } else {
            std::cerr << "[HeightmapStore] failed to save " << entry.files.height << "\n";
        }
        return ok;
    }
    return false;
}

int HeightmapStore::save_dirty() {
    int written = 0;
    for (auto& [key, entry] : entries_) {
        if (entry.data->dirty_on_disk() && save(entry.data.get())) ++written;
    }
    return written;
}

bool HeightmapStore::any_dirty() const {
    for (const auto& [key, entry] : entries_) {
        if (entry.data->dirty_on_disk()) return true;
    }
    return false;
}

void HeightmapStore::clear() { entries_.clear(); }

} // namespace world
} // namespace toy
