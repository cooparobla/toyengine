#include <toyengine/scene/terrain_digger.h>

#include <algorithm>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <toyengine/world/terrain_component.h>

namespace toy {
namespace scene {

void TerrainDigger::update(float delta_time) {
    if (cooldown_ > 0.0f) cooldown_ = std::max(0.0f, cooldown_ - delta_time);
    hovering_ = false;
    if (scene == nullptr || glm::length(ray_direction) < 1e-6f) return;

    // Digging moves tiles, so the target is a tile world -- not whichever terrain is first.
    world::TerrainComponent* terrain = nullptr;
    for (world::TerrainComponent* t : scene->get_components<world::TerrainComponent>()) {
        if (t->mode == world::TerrainMode::Tiles) { terrain = t; break; }
    }
    if (terrain == nullptr || !terrain->is_ready()) return;

    // Hover first, dig second: what the outline shows is exactly what a click would take.
    world::TileHit hit;
    if (!terrain->raycast_tiles(ray_origin, ray_direction, reach, hit)) return;
    hover_tile_ = hit.tile;
    const int r = std::max(0, radius);
    const world::IRect brush{hit.tile.x - r, hit.tile.y - r, hit.tile.x + r + 1, hit.tile.y + r + 1};
    // Lifted a little off the surface: an outline exactly on the ground it outlines reads as
    // part of the terrace rather than as a cue.
    hovering_ = terrain->tile_outline(brush, hover_corners_, 0.08f * terrain->params.height_step);

    if (!digging || cooldown_ > 0.0f) return;

    // The hit is in the terrain's own space; raise_at() wants a world position, and on a planet
    // the two differ by more than an offset.
    glm::vec3 world = hit.point;
    if (terrain->owner != nullptr) {
        if (auto* tc = terrain->owner->get_transform()) {
            world = glm::vec3(tc->transform().get_world_matrix() * glm::vec4(hit.point, 1.0f));
        }
    }
    const int delta = raising ? std::max(1, steps) : -std::max(1, steps);
    if (!terrain->raise_at(world, std::max(0, radius), delta)) return;

    last_dig_point_ = world;
    ++digs_;
    cooldown_ = std::max(0.0f, repeat_seconds);
}

} // namespace scene
} // namespace toy
