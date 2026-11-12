/**
 * @file destruction_system.h
 * @brief Runs destruction: fractures a Destructible's mesh on a worker and turns it into
 *        physical chunks when it breaks, and cuts a Sliceable or Destructible in two along a
 *        plane on demand (slice_object()).
 *
 * The policy half of destruction, split from the Destructible component the way TerrainSystem
 * is split from TerrainComponent, and for the same reasons: it needs the device, the allocator,
 * the asset manager, the job engine and the physics world, none of which a scene-authored
 * component should know about, and its work is per frame over every Destructible rather than
 * per object.
 *
 * What it does each pass, at UpdatePhase order **70** -- after terrain streaming (60) and
 * before physics (100), so a piece spawned this frame is simulated this frame rather than
 * hanging in the air for one:
 *
 *  1. **Fracture.** A Destructible with `prefracture` gets its mesh cut into cells on the job
 *     engine as soon as its mesh asset has loaded. The cells are kept on this system (not on
 *     the component), because they are geometry, and several objects sharing one mesh and one
 *     seed share one fracture -- a wall of fifty identical crates is cut once.
 *  2. **Break.** A Destructible whose `pending()` is set (a hard enough hit, or shatter())
 *     has its chunks spawned through spawn_piece_() (below). The original object is
 *     deactivated, not deleted, so whatever referenced it still resolves.
 *  3. **Clean up.** Pieces past their lifetime sink and are removed, so a scene that is broken
 *     over and over does not accumulate bodies for ever.
 *
 * **Slicing** (slice_object(), public) is the other way to turn one object into two: a plane
 * cut (toyengine/destruct/slice.h) rather than a Voronoi break, through the same spawn_piece_()
 * -- same mesh/collider/Rigidbody path, same cleanup, same cap-face material handling.
 *
 * **Typed damage.** A `Damageable` queues blows (an impact, a slash, ...); this system drains
 * the queue each pass, resolves each through physxcoopa's `damage::resolve_damage()`, deposits
 * it into the object's local `destruct::DamageField`, and carries out four of the five possible
 * responses: Fracture (the Destructible path above, if a sibling Destructible exists), Cleave
 * (slice_object(), once the field's saturated region reads flat enough to trust as a cut plane),
 * and Dent/Chip (apply_dent_(), below). Hole is not implemented -- see damageable.h's own doc.
 *
 * **Dents.** A Damageable's first Dent or Chip blow calls ensure_dent_mesh_(), which gives the
 * object a private, deformable clone of its mesh asset: the CPU vertices are read back from the
 * mesh FILE (`Mesh::build_cpu()`, the same reason ensure_fracture_() reads a file rather than
 * the asset manager -- one file cannot be two asset types at once), subdivided if it is coarser
 * than `Damageable::dent_resolution` (toyengine/destruct/dent.h's subdivide_for_dent() -- the
 * ONLY point this can happen, since a dynamic Mesh's index buffer is fixed for its life), built
 * as a `frames_in_flight`-buffered dynamic Mesh (Mesh::from_arrays()), published once under a
 * per-object synthetic asset id, and swapped onto the MeshRenderer (set_mesh()) in place of the
 * shared asset. From there apply_dent() (toyengine/destruct/dent.h) displaces it per blow. The
 * clone is kept on THIS system, not on Damageable, because building one needs the device,
 * allocator and asset manager Damageable deliberately does not carry (see its own file doc).
 *
 * The clone's vertex buffer is NOT re-uploaded only on the frames a dent lands: once an object
 * has one, upload_dents() rewrites it from `vertices_` into `frame_slot` EVERY frame, the same
 * contract ClothRenderer::upload() keeps (see that file's doc) -- `Mesh::update_vertices()`
 * pins `active_slot_` to whatever slot it last wrote, so a write only on dent frames could land
 * in a slot a still-in-flight frame is reading. Engine::upload_dynamic_meshes_() is the window;
 * a scene with nothing dented costs nothing here, since no object has joined `dents_`.
 *
 * `DESTRUCT_DEBUG=1` in the environment prints one line per break or slice (what broke, into
 * how many pieces) -- the first thing to look at when something refuses to come apart.
 *
 * **Piece colliders are convex hulls by default, boxes as the fallback.** spawn_piece_()
 * builds a hull from the piece's own vertices (`collider: hull`, physxcoopa's
 * `ConvexCollider`); a degenerate hull (a near-flat sliver -- a thin slice, a crushed shard) or
 * an explicit `collider: boxes` falls back to the piece's OWN best-fit box or boxes
 * (destruct::fit_boxes()), the only shape this system had before physxcoopa gained
 * `ShapeType::ConvexHull`. A shard that fills too little of its one box (`collider_fill`) is
 * cut into up to `max_boxes` parts in the box path only, spawned as child objects whose
 * colliders PhysicsSystem gathers into one compound body -- a wedge then rests on its face
 * instead of hanging off a corner of a box that is mostly air.
 *
 * A child collider's Transform must be parented to the piece's by hand (SceneObject::add_child
 * links ownership only): a compound's shapes are placed from the child's world transform
 * relative to the root's, so an unparented child puts its shape a whole piece position away
 * from the body, and the piece falls through the floor it should be resting on.
 */

#ifndef TOYENGINE_SCENE_DESTRUCTION_SYSTEM_H
#define TOYENGINE_SCENE_DESTRUCTION_SYSTEM_H

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <coopa/job/engine.h>
#include <coopa/scene/scene.h>
#include <gfxcoopa/engine/data/skinned_mesh_source.h>

#include <toyengine/destruct/dent.h>
#include <toyengine/destruct/fracture.h>

namespace coopa::asset { class AssetManager; }
namespace coopa::gfx::core { class Device; }
namespace coopa::gfx::memory { class Allocator; }
namespace coopa::gfx::engine::data { class Mesh; }
namespace coopa::physx::system { class PhysicsSystem; }
namespace coopa::physx::damage { struct DamageEvent; struct ResolvedDamage; }

namespace toy {
namespace scene {

class Destructible;
class Damageable;

/**
 * @class DestructionSystem
 * @brief Fractures breakable meshes and spawns their chunks (see the file doc).
 */
class DestructionSystem : public coopa::scene::ISceneSystem {
public:
    DestructionSystem(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                      coopa::asset::AssetManager& assets, coopa::job::JobEngine* jobs,
                      std::uint32_t frames_in_flight = 2)
        : device_(device), allocator_(allocator), assets_(assets), jobs_(jobs),
          frames_in_flight_(std::max(1u, frames_in_flight)) {}

    ~DestructionSystem() override;

    const char* system_name() const override { return "Destruction"; }
    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

    /** @brief Fractures done so far -- one per distinct mesh + settings (tests, diagnostics). */
    std::size_t fractures() const { return cache_.size(); }
    /** @brief Chunk objects alive right now. */
    std::size_t live_chunks() const { return chunks_.size(); }
    /** @brief Objects carrying a private, dented clone of their mesh right now (tests,
     *         diagnostics). */
    std::size_t dented_meshes() const { return dents_.size(); }

    /** @brief Rewrites every dented object's vertex buffer into `frame_slot` -- see this file's
     *         doc for why every frame, not just the ones a dent lands on. Called from
     *         Engine::upload_dynamic_meshes_(), the only window the in-flight slot is known in. */
    void upload_dents(std::uint32_t frame_slot);

    /**
     * @brief Cuts `object` along a world-space plane into two rigid halves.
     *
     * Transforms the plane into `object`'s local space (the inverse world matrix; the normal
     * by the inverse-TRANSPOSE, since a plain inverse skews a normal under non-uniform scale),
     * slices its MeshRenderer's mesh (toyengine/destruct/slice.h), spawns a piece per half
     * through the same spawn_piece_() a fracture chunk uses, deactivates the original like a
     * shatter does, and inherits its Rigidbody's linear and angular velocity onto both halves
     * so a moving object stays moving. `separation_speed` pushes each half away from the plane
     * (+-world_normal), so they part instead of interpenetrating.
     *
     * @return How many halves were spawned: 2 on a cut, 0 if the plane missed the mesh or
     *         either half came out below Sliceable::min_volume_fraction (or the object has
     *         neither a Sliceable nor a Destructible to carry the piece settings).
     */
    int slice_object(coopa::scene::Scene& scene, coopa::scene::SceneObject& object,
                     const glm::vec3& world_point, const glm::vec3& world_normal,
                     float separation_speed = 0.0f);

private:
    /// One mesh broken one way: the cells, and the job cutting them if it is still running.
    struct Fracture {
        std::shared_ptr<std::vector<destruct::FracturePiece>> pieces;
        coopa::job::JobHandle job;
        bool ready = false;
    };

    /// A chunk in flight: what to remove, and when.
    struct Chunk {
        coopa::scene::SceneObject* object = nullptr;
        float age = 0.0f;
        float lifetime = 0.0f;
        float fade = 0.0f;
    };

    /// One piece's geometry, in its own principal frame -- a FracturePiece or a SlicePiece,
    /// whichever made it. `boxes` is the fracture piece's pre-fit box compound; empty for a
    /// slice half, which fits its box compound on demand only if it ends up needing one.
    struct PieceSpawn {
        std::vector<destruct::Vertex>        vertices;
        std::vector<std::uint32_t>           indices;
        std::uint32_t                        cap_first_index = 0;
        glm::vec3                            centre{0.0f};
        glm::quat                            orientation{1.0f, 0.0f, 0.0f, 0.0f};
        glm::vec3                            half_extent{0.0f};
        float                                volume = 0.0f;
        std::vector<destruct::FractureBox>   boxes;
    };

    /// How to dress and weigh a spawned piece -- the settings a Destructible or Sliceable
    /// carries, independent of which one produced the geometry.
    struct PieceStyle {
        float       density = 700.0f;
        std::string physics_material;    ///< physics_materials/<name>.yaml; empty = engine default.
        std::string interior_material;   ///< materials/<name>.yaml for the cap; empty = reuse the surface.
        float       lifetime = 10.0f;
        float       fade = 1.0f;
        bool        use_hull = true;     ///< `collider: hull` (true) or `collider: boxes` (false).
        float       collider_fill = 0.6f;
        int         max_boxes = 4;
        glm::vec3   hit_point{0.0f};      ///< World space; the piece is pushed away from this.
        glm::vec3   hit_velocity{0.0f};   ///< Extra world-space velocity (already scaled), added as-is.
        float       scatter = 0.0f;       ///< Outward speed, m/s, away from `hit_point`.
        glm::vec3   initial_angular_velocity{0.0f}; ///< E.g. a sliced object's own spin, inherited.
    };

    /// An object's private, deformable clone of its mesh asset -- built once, on its first Dent
    /// or Chip, and mutated in place by every blow after.
    struct DentedMesh {
        std::shared_ptr<coopa::gfx::engine::data::Mesh> mesh;
        std::vector<destruct::Vertex>     vertices;
        std::vector<std::uint32_t>        indices;
        destruct::DentState               state;
        /// Damageable::max_dent's 0 default: 0.35 of the UNDENTED mesh's shortest world extent,
        /// computed once at build time so the cap does not shrink as the mesh itself does.
        float default_max_offset = 0.0f;
    };

    /// Keyed on the mesh and the settings that shape the break, so identical crates share one.
    static std::string cache_key_(const std::string& mesh_path, const Destructible& d);

    /** @brief Starts (or advances) the fracture a Destructible needs; true once its cells exist. */
    bool ensure_fracture_(Destructible& d, const std::string& mesh_path, Fracture*& out);

    /** @brief Spawns one rigid chunk per cell and deactivates the original. */
    int spawn_chunks_(coopa::scene::Scene& scene, Destructible& d, const Fracture& fracture);

    /**
     * @brief Spawns one rigid piece of `owner`: mesh upload (with a second material slot for
     *        the cap, when `style.interior_material` is set and the piece has one), a convex
     *        hull collider (falling back to one-to-several boxes when `style.use_hull` is
     *        false or the hull builder degenerates on a near-flat sliver), and a Rigidbody
     *        weighed from the piece's own volume. The outward push and any inherited motion go
     *        on `RigidbodyComponent::initial_velocity`/`initial_angular_velocity` rather than
     *        `add_impulse()` -- the body is not bound into PhysicsWorld until PhysicsSystem's
     *        next reconcile, so an impulse applied now, before that bind, would have nothing to
     *        act on and silently do nothing; the initial_* fields are read AT bind time for
     *        exactly this reason. Registers the piece for age_chunks_() cleanup and marks
     *        physics dirty; does not touch `owner`.
     */
    coopa::scene::SceneObject* spawn_piece_(coopa::scene::Scene& scene, coopa::scene::SceneObject& owner,
                                            const PieceSpawn& piece, const PieceStyle& style);

    /** @brief Ages chunks out: sink, then remove. */
    void age_chunks_(coopa::scene::Scene& scene, float dt);

    /** @brief Drains one Damageable's blow queue: resolves each against its material, deposits
     *         it into the local DamageField, and carries out Fracture/Cleave/Dent/Chip when a
     *         blow or the accumulated field calls for one (see this file's doc). */
    void process_damage_(coopa::scene::Scene& scene, Damageable& damageable);

    /** @brief Builds `damageable`'s dent clone on first call (see this file's doc); every call
     *         after returns the same one. Null if the owner has no MeshRenderer, no mesh asset
     *         (a slice-spawned half, say -- those have no file to read), the asset cannot be
     *         read, or its vertex count is past the dent limit -- in every case the blow still
     *         cost HP in process_damage_(), it just leaves the mesh alone. */
    DentedMesh* ensure_dent_mesh_(Damageable& damageable);

    /** @brief Pushes `damageable`'s dent clone in at `world_blow`'s point, sized and shaped by
     *         `resolved` (DamageResponse::Dent or Chip only -- see this file's doc). A no-op if
     *         `Damageable::dent` is off or ensure_dent_mesh_() could not build a clone. */
    void apply_dent_(Damageable& damageable, const coopa::physx::damage::DamageEvent& world_blow,
                     const coopa::physx::damage::ResolvedDamage& resolved, const glm::mat4& world,
                     const glm::mat4& inv_world);

    /** @brief Consumes every DamageCursor's pending slice/hit request: raycasts through
     *         `physics`, and on a hit calls slice_object() (slice_button) or queues a blow on
     *         the hit object's Damageable (hit_button). A miss, or a hit with no Damageable for
     *         hit_button, is a silent no-op -- the demo's own click, not an error. */
    void process_damage_cursors_(coopa::scene::Scene& scene, coopa::physx::system::PhysicsSystem& physics);

    coopa::gfx::core::Device&      device_;
    coopa::gfx::memory::Allocator& allocator_;
    coopa::asset::AssetManager&    assets_;
    coopa::job::JobEngine*         jobs_ = nullptr;
    std::uint32_t                  frames_in_flight_ = 2;

    std::unordered_map<std::string, Fracture> cache_;
    std::vector<Chunk> chunks_;
    std::uint64_t      spawned_ = 0;   ///< Names chunk objects and their meshes uniquely.
    std::unordered_map<const Damageable*, DentedMesh> dents_;
    std::uint64_t      next_dent_id_ = 0;  ///< Names dent clones' synthetic asset ids uniquely.
    bool               physics_dirty_ = false;
};

/**
 * @brief Installs a DestructionSystem on `scene` (idempotent); returns it.
 *
 * @param jobs Job engine for the fracture work; null fractures inline on the owner thread,
 *             which is what the device-free tests do.
 * @param frames_in_flight Buffer count a dent clone's dynamic Mesh is built with -- must match
 *                         the renderer's own, or upload_dents() would write into a slot an
 *                         in-flight frame is still reading.
 */
DestructionSystem* install_destruction_system(coopa::scene::Scene& scene,
                                              coopa::gfx::core::Device& device,
                                              coopa::gfx::memory::Allocator& allocator,
                                              coopa::asset::AssetManager& assets,
                                              coopa::job::JobEngine* jobs,
                                              std::uint32_t frames_in_flight = 2);

} // namespace scene
} // namespace toy

#endif // TOYENGINE_SCENE_DESTRUCTION_SYSTEM_H
