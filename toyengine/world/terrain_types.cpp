#include <toyengine/world/terrain_types.h>

namespace toy {
namespace world {

TerrainMode terrain_mode_from_name(const std::string& name) {
    if (name == "heightmap" || name == "Heightmap") return TerrainMode::Heightmap;
    return TerrainMode::Tiles;
}

const char* terrain_mode_name(TerrainMode mode) {
    return mode == TerrainMode::Heightmap ? "heightmap" : "tiles";
}

WaterKind water_kind_from_name(const std::string& name) {
    if (name == "ocean" || name == "sea") return WaterKind::Ocean;
    if (name == "lake") return WaterKind::Lake;
    if (name == "river") return WaterKind::River;
    return WaterKind::None;
}

const char* water_kind_name(WaterKind kind) {
    switch (kind) {
        case WaterKind::Ocean: return "ocean";
        case WaterKind::Lake:  return "lake";
        case WaterKind::River: return "river";
        case WaterKind::None:  break;
    }
    return "none";
}

} // namespace world
} // namespace toy
