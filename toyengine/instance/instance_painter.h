/**
 * @file instance_painter.h
 * @brief `InstancePainter` -- a bag of painted instances of one or more object assets, drawn as
 *        instanced meshes and (per prototype) collided.
 *
 * The hand-authored counterpart to the procedural foliage scatter
 * (toyengine/particles/terrain_scatter.h). A scatter is tuned: density, slope, a seed. A painter
 * is *placed*: the editor's Instance Paint mode brushes instances onto whatever surface is under
 * the cursor, and the painter stores exactly what was put down. Use a scatter for grass over a
 * whole landscape and a painter for the trees, rocks, fences and clutter that make a place.
 *
 * An instance is a 48-byte record (toyengine/instance/instance_set.h), not a `SceneObject`: a
 * world covered in them costs no transforms, no component walk and no scene-file growth. What it
 * gives up is per-instance components -- an instance that needs to be scripted is promoted to a
 * real `prefab:` object by the editor's Convert to Object.
 *
 * ### YAML
 *
 * @code
 * - type: InstancePainter
 *   data: instances/forest.instances   # default instances/<object>.instances, beside the scene
 *   chunk_size: 32                     # metres per chunk: the draw, cull, collide and file unit
 *   collision_radius: 64               # metres from the camera colliders exist within; 0 = none
 *   cast_shadows: true
 *   prototypes:
 *     - object: props/pine             # an object asset -- its meshes and bounds are read from it
 *       collision: auto                # none | auto | box | sphere | capsule | mesh
 *       scale_range: [0.85, 1.3]
 *       yaw_range: [0, 360]            # degrees
 *       align_to_normal: 0.3           # 0 world up .. 1 fully along the surface normal
 *       sink: 0.05                     # metres pushed into the surface
 *       draw_distance: 0               # 0 = automatic, from the prototype's height
 *       fade_start: 0.6                # thinning begins at this fraction of the draw distance
 *     - mesh: rock_small               # ...or a bare mesh plus a material
 *       material: { albedo: { r: 0.5, g: 0.5, b: 0.52 }, roughness: 0.9 }
 * @endcode
 *
 * ### What lives where
 *
 * The component holds only configuration. The instances live in an `InstanceSetData` shared
 * through the `InstanceSetStore`, keyed by the `.instances` file -- not in the component, and
 * never in the scene file. That is load bearing: the editor re-parses a component from YAML on
 * every inspector keystroke, and anything held here would be lost on each one.
 *
 * `InstancePainterSystem` (order 62) owns everything derived: the per-chunk instance matrices,
 * the render batches and the streamed colliders.
 */

#ifndef TOYENGINE_INSTANCE_INSTANCE_PAINTER_H
#define TOYENGINE_INSTANCE_INSTANCE_PAINTER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>

#include <toyengine/instance/instance_set.h>

namespace toy {
namespace instance {

/** @brief What a prototype's instances collide as. */
enum class InstanceCollision : int {
    None = 0,  ///< Visual only. Grass, flowers, pebbles: no physics cost at all.
    /**
     * @brief A capsule when the prototype is taller than wide, else a box -- fitted to the
     *        WHOLE prototype.
     *
     * Right for a crate, a rock, a barrel. Wrong for anything whose silhouette is much wider
     * than the part that should stop you: a tree fitted this way gets a trunk as wide as its
     * canopy. Author `collision_radius` (or `collision_size`) in that case.
     */
    Auto,
    Box,
    Sphere,
    Capsule,
    Mesh,      ///< The prototype's `<mesh>_collider` asset if it has one, else its render mesh.
};

/** @brief Parses a `collision:` value; false (and `out` untouched) on an unknown name. */
bool parse_instance_collision(const std::string& name, InstanceCollision& out);

/** @brief The name `parse_instance_collision()` accepts for `c`. */
const char* instance_collision_name(InstanceCollision c);

/**
 * @struct InstancePrototype
 * @brief One entry in a painter's palette: what gets painted, and how each instance of it varies.
 *
 * The authored fields come from YAML. The resolved ones below them are filled in once, lazily,
 * by `resolve_prototype()` -- which instantiates the object asset into a template that is never
 * added to the scene and never drawn, purely to read its meshes and bounds out.
 */
struct InstancePrototype {
    // --- authored ---

    /** @brief An object asset (`props/pine`). Mutually exclusive with `mesh`. */
    std::string object;
    /** @brief A bare mesh (`rock_small`), used when `object` is empty. */
    std::string mesh;
    /** @brief The material for a `mesh` prototype (an `object` one brings its own). */
    coopa::gfx::engine::components::PBRMaterial material;

    InstanceCollision collision = InstanceCollision::None;
    /** @brief Explicit collider extents; 0 fits them to the prototype's bounds. */
    glm::vec3 collision_size{0.0f};
    /** @brief Explicit collider radius (Sphere, Capsule); 0 fits it to the bounds. */
    float collision_radius = 0.0f;
    /**
     * @brief `collision: mesh` only: the mesh asset the triangle collider is built from.
     *
     * Empty uses the prototype's own render mesh. Name a simplified `<thing>_collider` here
     * when the render mesh is too dense to collide against -- it is spelled out rather than
     * guessed, so a missing collider mesh is a visible authoring choice and never a silent
     * fallback to a 20k-triangle tree.
     */
    std::string collision_mesh;
    std::string physics_material;

    /** @brief Uniform scale drawn per instance. */
    glm::vec2 scale_range{1.0f, 1.0f};
    /** @brief Yaw about the placement normal, degrees. */
    glm::vec2 yaw_range{0.0f, 360.0f};
    /** @brief 0 stands every instance up; 1 tilts it fully onto the surface normal. */
    float align_to_normal = 0.0f;
    /** @brief Metres pushed into the surface, so a trunk is not floating on a slope. */
    float sink = 0.0f;
    /** @brief Metres past which instances are not drawn; 0 derives one from the height. */
    float draw_distance = 0.0f;
    /** @brief Thinning starts at this fraction of `draw_distance`. */
    float fade_start = 0.6f;
    bool cast_shadows = true;
    /** @brief A short label for the editor's palette; the asset name when empty. */
    std::string name;

    // --- resolved (instance_prototype.cpp) ---

    /** @brief One drawable piece of the prototype: a mesh + material, and where it sits in it. */
    struct Part {
        coopa::gfx::engine::components::MeshRenderer* proxy = nullptr;  ///< Owned by this prototype.
        glm::mat4 local{1.0f};                                         ///< Relative to the prototype root.
    };

    std::vector<Part> parts;
    /** @brief Local-space bounds over every part, for collider fitting and draw distance. */
    glm::vec3 bounds_min{0.0f}, bounds_max{0.0f};
    /** @brief Resolution ran (successfully or not); `parts` empty means it found nothing. */
    bool resolved = false;
    /** @brief A warning has been logged for this prototype; do not log it again. */
    bool warned = false;

    /** @brief What this prototype is called in logs and in the editor's palette. */
    std::string label() const;

    /** @brief Every part's mesh has loaded, so a batch built from it can draw. */
    bool ready() const;

    /** @brief Height in metres at the largest authored scale -- what draw distance derives from. */
    float nominal_height() const;

    // Held by pointer so the struct stays movable without the SceneObject / MeshRenderer
    // definitions mattering to every includer.
    /** @brief The instantiated object asset the parts point into; null for a `mesh` prototype. */
    std::shared_ptr<coopa::scene::SceneObject> templ;
    /** @brief A `mesh` prototype's single proxy, which nothing else owns. */
    std::shared_ptr<coopa::gfx::engine::components::MeshRenderer> own_proxy;
};

/**
 * @class InstancePainter
 * @brief See the file comment.
 */
class InstancePainter : public coopa::scene::Component {
public:
    std::string type_name() const override { return "InstancePainter"; }

    /** @brief The `.instances` file, as authored; default `instances/<object>.instances`. */
    std::string data_file;

    /**
     * @brief The painted instances. Shared through `InstanceSetStore`, so re-parsing the
     *        component keeps the live (possibly unsaved) records. Set by the parser.
     */
    std::shared_ptr<InstanceSetData> data;

    std::vector<InstancePrototype> prototypes;

    /** @brief Metres per chunk edge. Applied to `data` by the parser. */
    float chunk_size = InstanceSetData::k_default_chunk_size;

    /**
     * @brief Metres from the camera within which collidable instances have colliders.
     *
     * 0 disables streaming entirely (nothing collides, whatever a prototype asks for). The
     * default is deliberately modest: each collidable instance inside it costs one runtime
     * SceneObject and one static body.
     */
    float collision_radius = 64.0f;

    /** @brief Off here turns shadow casting off for every prototype, whatever each asks. */
    bool cast_shadows = true;

    /** @brief Instances are drawn and collided; off leaves the records untouched but invisible. */
    bool enabled = true;

    /** @brief The prototype at `index`, or null when it is out of range. */
    const InstancePrototype* prototype_at(std::size_t index) const {
        return index < prototypes.size() ? &prototypes[index] : nullptr;
    }

    /** @brief The index of the prototype named `name`, or -1. */
    int find_prototype(const std::string& name) const;
};

/**
 * @brief Fills in a prototype's `parts` and `bounds_*`, once.
 *
 * An `object` prototype is instantiated through the ordinary prefab path
 * (`SceneLoader::instantiate`), which brings nested prefabs, material slots and LODs along, and
 * then walked for its `MeshRenderer`s. The template is never parented into a scene and never
 * started, so nothing in it runs: it exists to be read.
 *
 * Idempotent, and safe to call before the meshes have loaded -- `InstancePrototype::ready()` is
 * the separate question of whether a batch can draw yet.
 *
 * @param p The prototype to resolve.
 * @param log_warning Called with one line when the prototype is unusable or surprising (an
 *        object asset carrying behaviour components, a reference that does not resolve).
 * @return True when it ended up with at least one part.
 */
bool resolve_prototype(InstancePrototype& p, const std::function<void(const std::string&)>& log_warning);

/**
 * @brief Recomputes a prototype's local-space bounds from whichever of its parts have loaded.
 *
 * Separate from `resolve_prototype()` because meshes load asynchronously: the parts are known
 * immediately, their bounds only once the files land. The system calls this on the frame the
 * prototype first becomes `ready()`.
 */
void refresh_prototype_bounds(InstancePrototype& p);

/** @brief Registers the "InstancePainter" parser. Called once, at start-up. */
void register_instance_components(coopa::asset::AssetManager& assets);

} // namespace instance
} // namespace toy

#endif // TOYENGINE_INSTANCE_INSTANCE_PAINTER_H
