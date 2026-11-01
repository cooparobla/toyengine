#include "terrain_common.h"

#include <toyengine/world/terrain_yaml.h>

namespace toy::editor {

namespace {
constexpr const char* kTerrainNames[] = {"Terrain", "TileTerrain", "HeightmapTerrain"};
}

bool is_any_terrain_object(const SceneDocument& doc, ObjectId id) {
    for (const char* name : kTerrainNames) {
        if (doc.find_component(id, name) >= 0) return true;
    }
    return false;
}

std::optional<toy::world::TerrainMode> terrain_mode_of(const SceneDocument& doc, ObjectId id) {
    const Node* obj = doc.find(id);
    if (!obj || !obj->contains("components")) return std::nullopt;
    const auto& seq = obj->at("components").as_seq();
    for (const char* name : kTerrainNames) {
        const int idx = doc.find_component(id, name);
        if (idx < 0) continue;
        return toy::world::infer_terrain_mode(seq[static_cast<size_t>(idx)]);
    }
    return std::nullopt;
}

int find_tile_terrain_component(const SceneDocument& doc, ObjectId id) {
    const Node* obj = doc.find(id);
    if (!obj || !obj->contains("components")) return -1;
    const auto& seq = obj->at("components").as_seq();
    for (const char* name : kTerrainNames) {
        const int idx = doc.find_component(id, name);
        if (idx < 0) continue;
        return toy::world::infer_terrain_mode(seq[static_cast<size_t>(idx)]) == toy::world::TerrainMode::Tiles ? idx : -1;
    }
    return -1;
}

} // namespace toy::editor
