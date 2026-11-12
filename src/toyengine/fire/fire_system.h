/**
 * @file fire_system.h
 * @brief Drives `Burnable`: advances a burn front over an object, and burns it out.
 *
 * Runs at order **44**. It reads nothing but its own components -- no weather, no environment
 * field, no terrain, no world fire state of any kind. The order is kept only so burning is
 * resolved before gameplay behaviours (200) read `Burnable::progress()`.
 *
 * What it does each frame:
 *
 * **Burnable** -- advances the front outward at `spread_rate`, publishes it to the object's
 *    materials for `gfx/surface/burn.glsl`, moves the flame emitters out with it, and applies
 *    `on_burnt` once `burn_time` is up.
 *
 * The front is published through `PBRMaterial::shader_params` as `(origin_ws.xyz, radius)` and
 * `PBRMaterial::burning`. **The system owns both for as long as an object is alight**, so a
 * material that uses `shader_params` for its own surface shader must not also be burnable.
 */
#ifndef TOYENGINE_FIRE_FIRE_SYSTEM_H
#define TOYENGINE_FIRE_FIRE_SYSTEM_H

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <toyengine/particles/particle_shape.h>

#include <toyengine/fire/fire.h>

namespace toy {
namespace fire {

/// Update order. Before gameplay behaviours (200), which may read progress().
constexpr int k_fire_system_order = 44;

/**
 * @class FireSystem
 * @brief The scene system behind `Burnable`. See the file doc.
 */
class FireSystem : public coopa::scene::ISceneSystem {
public:
    const char* system_name() const override { return "Fire"; }
    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;


    /**
     * @brief Lights the nearest Burnable the ray hits, at the hit point.
     *
     * The pick is a ray against each candidate's mesh bounding box in its own local space, which
     * is precise enough to tell a chair's back from its seat and needs no collider, no physics
     * world and no CPU copy of the mesh.
     *
     * @param scene The scene to search.
     * @param origin Ray origin, world space.
     * @param direction Ray direction, world space; need not be normalised.
     * @param max_distance Metres to search.
     * @return The Burnable that was lit, or null if the ray hit nothing burnable.
     */
    Burnable* ignite_ray(coopa::scene::Scene& scene, const glm::vec3& origin,
                         const glm::vec3& direction, float max_distance = 1000.0f);

    /** @brief Puts every Burnable in the scene back to untouched. */
    void reset(coopa::scene::Scene& scene);

    /**
     * @brief Supplies the per-mesh sampling surfaces the burn-line emitters are built from.
     *
     * Required for smoke and flame to appear ON a burning object: the emitters sample its own
     * triangles, and `ParticleSystem::load_surface` is only ever wired up by the YAML parser,
     * so a runtime-built emitter has nobody to fill it in.
     */
    void set_surface_loader(
        std::function<std::shared_ptr<const particles::MeshSurface>(const std::string&)> loader) {
        surface_loader_ = std::move(loader);
    }

    /** @brief How many objects are burning right now. */
    std::size_t burning_count() const { return burning_count_; }




private:

    void step_burnables_(coopa::scene::Scene& scene, float dt);
    /// Bakes every part of `obj` into ONE sampling surface in the object's own space.
    std::shared_ptr<const particles::MeshSurface> bake_surface_(coopa::scene::SceneObject* obj);
    /// Builds the burn-line emitters for a newly lit object.
    coopa::scene::SceneObject* make_burn_effects_(coopa::scene::Scene& scene, Burnable& b);
    void drop_burn_effects_(Burnable& b);
    void finish_burn_(Burnable& b);

    void set_material_burn_(coopa::scene::SceneObject* obj, bool burning, const glm::vec4& front);


    std::size_t burning_count_ = 0;
    coopa::scene::SceneObject* root_ = nullptr;
    std::function<std::shared_ptr<const particles::MeshSurface>(const std::string&)> surface_loader_;
};

/**
 * @brief Installs the system on `scene`, or returns the one already there.
 * @param scene The scene.
 * @param order The update order; the default is what every consumer assumes.
 */
FireSystem* install_fire_system(coopa::scene::Scene& scene, int order = k_fire_system_order);

/** @brief The scene's fire system, or null. */
FireSystem* find_fire(coopa::scene::Scene& scene);

} // namespace fire
} // namespace toy

#endif // TOYENGINE_FIRE_FIRE_SYSTEM_H
