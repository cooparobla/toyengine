#include <toyengine/world/terra_store.h>

#include <algorithm>
#include <iostream>

#include <toyengine/world/terra_codec.h>

namespace toy {
namespace world {

TerraStore& TerraStore::instance() {
    static TerraStore store;
    return store;
}

std::filesystem::file_time_type TerraStore::mtime_(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

void TerraStore::load_entry_(Entry& entry, const TerraGeometry& geometry) {
    entry.data->reset(geometry);
    // A missing or unreadable file leaves the fresh grid reset() made -- needs_bake() is already
    // true, so a MapGeneratorComponent (or a hand-sculpted start) fills it from here.
    load_terra(entry.path, *entry.data);
    entry.data->set_saved();
    entry.mtime = mtime_(entry.path);
}

std::shared_ptr<TerraData> TerraStore::acquire(const std::string& path, const TerraGeometry& geometry_in) {
    // A caller with no opinion on the tile count yet (mapcoopa decides at bake time, for a Tiles-
    // mode terrain) passes 0: this keeps whatever is already loaded instead of reshaping a baked
    // world down to nothing every time sync_field() re-derives a geometry from `params` alone.
    const bool keep_size = geometry_in.tiles_x <= 0 || geometry_in.tiles_y <= 0;
    TerraGeometry geometry = geometry_in.sanitized();
    auto it = entries_.find(path);
    if (it == entries_.end()) {
        Entry entry;
        entry.path = path;
        entry.data = std::make_shared<TerraData>(geometry);
        load_entry_(entry, geometry);
        it = entries_.emplace(path, std::move(entry)).first;
        return it->second.data;
    }

    Entry& entry = it->second;
    TerraData& data = *entry.data;

    // Edited elsewhere (another tool, or the editor process saving while a game runs).
    if (!data.dirty_on_disk() && mtime_(path) != entry.mtime) {
        entry.shapes.clear();
        load_entry_(entry, geometry);
        return entry.data;
    }

    if (keep_size) {
        geometry.tiles_x = data.geometry().tiles_x;
        geometry.tiles_y = data.geometry().tiles_y;
    }
    if (data.geometry() != geometry) {
        // Remember the current shape, then restore the requested one exactly if we have it.
        auto cached = std::find_if(entry.shapes.begin(), entry.shapes.end(),
                                   [&](const TerraData& d) { return d.geometry() == geometry; });
        TerraData previous = data;
        if (cached != entry.shapes.end()) {
            data.assign_content(*cached);
            entry.shapes.erase(cached);
        } else {
            data.reshape(geometry);
        }
        entry.shapes.push_back(std::move(previous));
        if (entry.shapes.size() > 4) entry.shapes.erase(entry.shapes.begin());
    }
    return entry.data;
}

std::string TerraStore::path_of(const TerraData* data) const {
    for (const auto& [key, entry] : entries_) {
        if (entry.data.get() == data) return entry.path;
    }
    return {};
}

bool TerraStore::save(const TerraData* data) {
    for (auto& [key, entry] : entries_) {
        if (entry.data.get() != data) continue;
        const bool ok = save_terra(entry.path, *entry.data);
        if (ok) {
            entry.data->set_saved();
            entry.mtime = mtime_(entry.path);
        } else {
            std::cerr << "[TerraStore] failed to save " << entry.path << "\n";
        }
        return ok;
    }
    return false;
}

int TerraStore::save_dirty() {
    int written = 0;
    for (auto& [key, entry] : entries_) {
        if (entry.data->dirty_on_disk() && save(entry.data.get())) ++written;
    }
    return written;
}

bool TerraStore::any_dirty() const {
    for (const auto& [key, entry] : entries_) {
        if (entry.data->dirty_on_disk()) return true;
    }
    return false;
}

void TerraStore::clear() { entries_.clear(); }

} // namespace world
} // namespace toy
