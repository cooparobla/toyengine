/**
 * @file world_bake.cpp
 * @brief See world_bake.h.
 */

#include <toyengine/mapgen/world_bake.h>

#include <utility>

namespace toy {
namespace world {
namespace {

WorldBakeHook& slot() {
    static WorldBakeHook hook;
    return hook;
}

}   // namespace

void set_world_bake_hook(WorldBakeHook hook) { slot() = std::move(hook); }

const WorldBakeHook& world_bake_hook() { return slot(); }

}   // namespace world
}   // namespace toy
