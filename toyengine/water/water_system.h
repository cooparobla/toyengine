/**
 * @file water_system.h
 * @brief Bakes every WaterBody's surface (GPU mesh + CPU query) and drives every Buoyancy body
 *        from inside the physics substep.
 *
 * Ordering. Installed at k_water_system_order (90): just ahead of Physics (100). execute()
 * refreshes the list of buoyant bodies right before PhysicsSystem steps, so the pointers the
 * substep callback walks are never older than the current frame (a component destroyed during
 * the Behaviour walk or the command flush is dropped from the list before the next step).
 *
 * Bake. A body is baked in up to two stages:
 *  1. As soon as its geometry exists -- so frame 0 already draws water, and so does the editor
 *     (this system runs in edit mode, baking only) -- with whatever physics knows. Before
 *     PhysicsSystem has gathered colliders (it runs after this system, and not at all in edit
 *     mode) that bake has no depth or obstacle information.
 *  2. Once the scene simulates and PhysicsSystem has bodies, again WITH raycasts against static
 *     colliders: per-vertex
 *     water depth (calms waves toward the shore, see water_waves.h) and, for flowing water,
 *     obstacle deflection and wakes (water_flow_bake.h). The mesh is re-uploaded once.
 * A body whose Transform moves is re-baked (water bodies are expected to be static; this keeps
 * an editor drag correct rather than fast). Stage 2 is the expensive one (raycasts per vertex):
 * it waits until the body is within WaterSettings::sim_radius of the focus and runs for at most
 * WaterSettings::bakes_per_frame bodies a frame, nearest first, so a big world neither hitches
 * on its first simulated frame nor bakes lakes nobody is near.
 *
 * Quality and range (water_settings.h). set_settings() picks a tier. Distances are measured from
 * the focus: set_focus(), else the main camera of this scene, else none (no focus = everything
 * is in range, as in a headless test with no camera). Floaters outside sim_radius are frozen
 * (see substep_()); ripples come only from bodies within ripple_range.
 *
 * Tiles. A body larger than one render tile (WaterBody::tile_size; by default 32 m, at most
 * k_max_tiles_per_side a side) is published as runtime CHILD objects, one MeshRenderer per tile
 * with its own LOD chain (water_tiles.h), so the renderer culls and LODs each tile -- the
 * pattern TerrainSystem uses for its chunks. A single-tile body keeps its mesh on its own
 * MeshRenderer. Tile renderers copy the owner MeshRenderer's material on every bake, and every
 * frame in edit mode (call sync_material() after changing it from gameplay). Tiles are not part
 * of the authored scene.
 *
 * GPU vertex packing (consumed by assets/shaders/water_surface.glsl):
 *   position  object space, undisplaced
 *   normal    object space surface normal
 *   uv        (depth metres, turbulence 0..1)
 *   tangent   (object-space flow velocity m/s, 2) -- w == 2 marks a baked water vertex
 * plus PBRMaterial::shader = "water", shader_params = WaveParams::pack(),
 * shader_params_ext = (foam rgb, foam amount), (shore foam depth, edge fade depth,
 * ripple strength, ripple scale), and refraction_thickness = clarity (the hook overwrites the
 * refraction thickness with the measured depth, so the slot is free to carry it).
 *
 * Ripples. Every frame, each Rigidbody crossing a surface may emit an expanding ring (splash on
 * entry, a wake trail while moving, a ring now and then while bobbing) -- see emit_ripples_().
 * The live rings (ripples()) are handed to the renderer by Engine and drawn by the water shader;
 * they are visual only (buoyancy does not feel them).
 *
 * Underwater. underwater_at() answers whether a point -- the camera -- is below a surface; Engine
 * turns that into ToyRenderPipeline's UnderwaterPass parameters.
 *
 * Buoyancy. One connection to PhysicsWorld::on_substep for the whole scene. on_substep fires
 * after integrate_forces() and before the solve (world.h's step_fixed()), so buoyancy is applied
 * as IMPULSES (Body::apply_impulse_at_position) to reach this substep's solve; a force would land
 * one substep late. Impulses are applied without waking the body so a crate floating on calm
 * water can still fall asleep; a sleeping body is only woken when the water under it is moving
 * (waves or current) -- otherwise it stays asleep at zero cost.
 */

#ifndef TOYENGINE_WATER_WATER_SYSTEM_H
#define TOYENGINE_WATER_WATER_SYSTEM_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>


#include <gfxcoopa/engine/components/mesh_renderer.h>

#include <physxcoopa/system/physics_system.h>

#include <toyengine/water/buoyancy.h>
#include <toyengine/water/water_body.h>
#include <toyengine/water/water_flow_bake.h>
#include <toyengine/water/water_settings.h>

namespace toy {
namespace water {

/** @brief Default registration order: just ahead of UpdatePhase::Physics (100). See file doc. */
inline constexpr int k_water_system_order = 90;

/** @brief Render tile edge (m) when WaterBody::tile_size is 0. See the file doc. */
inline constexpr float k_default_tile_size = 32.0f;

/**
 * @brief Auto tile size never cuts a body into more than this many tiles a side. Every tile is a
 *        renderer the CPU gathers, culls, LOD-selects and sorts each frame, plus a transform to
 *        resolve: measured on the water_stress ocean (1 km), ~1000 tiles of 32 m cost ~3 ms of
 *        CPU a frame over one mesh for no GPU gain, while 64 tiles cost a fraction of that.
 */
inline constexpr int k_max_tiles_per_side = 8;

/**
 * @class WaterSystem
 * @brief See the file doc.
 */
class WaterSystem : public coopa::scene::ISceneSystem {
public:
    /** @brief GPU arguments may be null (headless tests): bodies are then baked CPU-side only. */
    WaterSystem(coopa::gfx::core::Device* device = nullptr, coopa::gfx::memory::Allocator* allocator = nullptr,
                coopa::asset::AssetManager* assets = nullptr)
        : device_(device), allocator_(allocator), assets_(assets) {}

    ~WaterSystem() override { substep_connection_.disconnect(); }

    const char* system_name() const override { return "Water"; }

    /// Runs in the editor too, but only to BAKE: water bodies need their mesh published to be
    /// visible at all, simulating or not. Buoyancy and ripples stay off until the scene
    /// simulates (see execute()).
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

    // ------------------------------------------------------------------------------------
    // Quality and range
    // ------------------------------------------------------------------------------------

    /** @brief Applies a quality tier (live). Procedural grids re-bake at the new density. */
    void set_settings(const WaterSettings& settings);
    const WaterSettings& settings() const { return settings_; }

    /** @brief Measures ranges from `p` instead of the main camera (until clear_focus()). */
    void set_focus(const glm::vec3& p);
    void clear_focus() { focus_override_ = false; }
    /** @brief The point ranges were measured from on the last execute(); false = no focus. */
    bool focus(glm::vec3& out) const;

    /** @brief True when every render tile of `body` (or its own MeshRenderer) has a mesh. */
    static bool is_published(const WaterBody& body);

    /** @brief Copies the owner MeshRenderer's material, look flags and tessellation to every render tile. */
    static void sync_material(WaterBody& body);

    /**
     * @brief Re-reads the scene's WaterBody list. sample()/underwater_at() walk the list taken at
     *        the last execute(); anything that may destroy or rebuild water objects between then
     *        and a query (the editor applies its edits after Scene::update()) must refresh first,
     *        or the query would read a destroyed component. Engine does, before every render.
     */
    void refresh_bodies(coopa::scene::Scene& scene);

    /** @brief Water clock (seconds) -- the time every wave on the CPU is evaluated at. */
    float time() const { return time_; }

    /**
     * @brief The water clock as the renderer should draw waves at -- the shader's wave phase
     *        (Engine hands it over as WaterFrameState::time). While physics runs, rendered bodies
     *        are interpolated a fraction of a substep behind the last solve, so the drawn surface
     *        is pulled back by the same amount; otherwise it is time() itself.
     */
    float render_time() const;

    /**
     * @brief Samples the highest water surface above world XY `p` across every water body.
     * @param which Optional: the body that answered.
     * @return false if no body covers `p`.
     */
    bool sample(const glm::vec2& p, WaterSample& out, const WaterBody** which = nullptr) const;

    /** @brief Number of bodies buoyancy considered on the last substep (diagnostics/tests). */
    std::size_t active_buoyant_count() const { return active_count_; }

    // ------------------------------------------------------------------------------------
    // Ripples
    // ------------------------------------------------------------------------------------

    /** @brief One expanding ring on a water surface. See emit_ripples_() for the sources. */
    struct Ripple {
        glm::vec2 position{0.0f};  ///< World XY of the ring's centre.
        float     birth = 0.0f;    ///< time() at emission.
        float     strength = 0.0f; ///< Peak slope scale (shader) -- ~0.05 a nudge, ~1 a splash.
        float     radius = 0.0f;   ///< Starting radius: the emitter's waterline size.
    };
    /// Hard cap on live rings (the renderer's UBO size); the tier's WaterSettings::max_ripples
    /// is the working cap (oldest dropped first): the shader loops over all of them.
    static constexpr std::size_t k_max_ripples = 64;
    /// Seconds a ring lives; its amplitude has decayed to ~2% by then.
    static constexpr float k_ripple_lifetime = 3.0f;

    /** @brief Live rings, oldest first. */
    const std::vector<Ripple>& ripples() const { return ripples_; }

    /** @brief Adds a ring by hand (gameplay: a thrown stone, a footstep). */
    void emit_ripple(const glm::vec2& position, float strength, float radius);

    // ------------------------------------------------------------------------------------
    // Underwater
    // ------------------------------------------------------------------------------------

    /** @brief Where `p` sits relative to the water: see underwater_at(). */
    struct UnderwaterInfo {
        bool             underwater = false;
        float            surface_height = 0.0f; ///< Of the body above `p` (valid when `body`).
        float            depth = 0.0f;          ///< surface_height - p.z (negative above water).
        const WaterBody* body = nullptr;        ///< The body above/around `p`, or null.
    };

    /**
     * @brief Whether world point `p` (e.g. a camera) is under a water surface -- the highest
     *        surface above its XY, waves included -- and above that body's bed.
     */
    UnderwaterInfo underwater_at(const glm::vec3& p) const;

private:
    // ------------------------------------------------------------------------------------
    // Physics hookup
    // ------------------------------------------------------------------------------------

    void connect_physics_(coopa::scene::Scene& scene);

    // ------------------------------------------------------------------------------------
    // Focus and range
    // ------------------------------------------------------------------------------------

    /** @brief The focus: set_focus()'s point, else this scene's main camera, else none. */
    void resolve_focus_(coopa::scene::Scene& scene);

    /** @brief Distance from the focus to a baked body's bounds (0 inside, or with no focus). */
    float distance_to_body_(const WaterBody& body) const;

    /** @brief How CPU wave samples are taken: the tier's inversion steps, faded like the GPU. */
    WaveQueryOptions wave_options_() const;

    // Minimum range sizes worth spreading over the job workers -- below them the dispatch costs
    // more than the work. Per item: a few multiplies (per-vertex packing), a wave sample per
    // pontoon (a floater), two raycasts (a depth probe), a whole tile.
    static constexpr std::size_t k_parallel_vertices = 65536;
    static constexpr std::size_t k_parallel_floaters = 32;
    static constexpr std::size_t k_parallel_rays     = 256;
    static constexpr std::size_t k_parallel_tiles    = 2;

    /**
     * @brief A ParallelFor (water_parallel.h) over this frame's job workers for ranges of at
     *        least `min_items`; smaller ranges, and all of them without a job engine, run inline.
     *        Every user writes only to its own indices, so the result is identical either way.
     */
    ParallelFor parallel_(std::size_t min_items) const;

    /** @brief Bodies near enough to the focus that a simulated floater could touch them. */
    void gather_active_bodies_();

    // ------------------------------------------------------------------------------------
    // Bake
    // ------------------------------------------------------------------------------------

    /** @brief Whether `body` is a procedural grid (no mesh geometry of its own). */
    static bool uses_grid_(const WaterBody& body) {
        return body.geometry_indices.empty() && body.mesh_path.empty();
    }

    /** @brief A procedural body's grid quads per side at `density`. */
    static int grid_resolution_(const WaterBody& body, float density);

    /** @brief The body's local-space triangle soup: its mesh source or a procedural grid of
     *         `grid_res` quads per side (set to -1 for mesh geometry). */
    static bool local_geometry_(const WaterBody& body, float density, std::vector<glm::vec3>& positions,
                                std::vector<glm::vec2>& uvs, std::vector<uint32_t>& indices, int& grid_res,
                                const ParallelFor* par = nullptr);

    /** @brief First STATIC, non-trigger hit along a ray; `out.normal` is the surface's own
     *         (outward / face) normal, NOT flipped toward the ray. */
    bool static_raycast_raw_(const glm::vec3& origin, const glm::vec3& dir, float max_distance,
                             FlowRayHit& out) const;

    /** @brief static_raycast_raw_() with the normal flipped to face back along the ray. */
    bool static_raycast_(const glm::vec3& origin, const glm::vec3& dir, float max_distance,
                         FlowRayHit& out) const;

    /** @brief Water depth below `p`: 0 when buried under a bank, max_depth over open water. */
    float probe_depth_(const glm::vec3& p, float max_depth) const;

    void bake_(coopa::scene::Scene& scene, WaterBody& body, const glm::mat4& world, bool with_physics);

    /** @brief The baked vertex stream (see the file doc's packing), object space. */
    static std::vector<coopa::gfx::engine::data::Vertex>
    make_vertices_(const glm::mat4& world, const std::vector<glm::vec3>& local_pos, const std::vector<uint32_t>& indices,
                   const std::vector<glm::vec3>& flow_ws, const std::vector<float>& depth,
                   const std::vector<float>& turbulence, const ParallelFor* par = nullptr, bool flat = false);

    /**
     * @brief Splits the baked surface into render tiles (water_tiles.h) and publishes them: one
     *        tile onto the owner's MeshRenderer, several as runtime child objects (see the file
     *        doc). `grid_res` >= 1 marks a row-major procedural grid of that many quads a side.
     */
    void publish_gpu_mesh_(coopa::scene::Scene& scene, WaterBody& body, const glm::mat4& world,
                           const std::vector<glm::vec3>& local_pos, const std::vector<uint32_t>& indices,
                           const std::vector<glm::vec3>& flow_ws, const std::vector<float>& depth,
                           const std::vector<float>& turbulence, int grid_res);

    /** @brief Destroys the tile objects of `body` not carried into `keep`, and makes `keep` its tiles. */
    void retire_tiles_(WaterBody& body, std::vector<WaterBody::TileSlot> keep);


    static bool active_in_hierarchy_(const coopa::scene::SceneObject& obj);

    static coopa::gfx::engine::components::MeshRenderer* add_default_renderer_(WaterBody& body);

    static void apply_material_(WaterBody& body);

    // ------------------------------------------------------------------------------------
    // Buoyancy
    // ------------------------------------------------------------------------------------

    /** @brief Per-Rigidbody ripple bookkeeping, keyed by component, pruned every frame. */
    struct RippleEmitter {
        float     radius = -1.0f;      ///< Bounding radius about the COM; < 0 = not computed yet.
        bool      touching = false;    ///< Crossed the surface last frame.
        glm::vec2 last_position{0.0f}; ///< Where it last emitted.
        float     last_time = -1e9f;
        bool      seen = false;
    };

    /** @brief Bounding radius of a Rigidbody's (non-trigger) colliders about its centre of mass. */
    static float bounding_radius_(const coopa::physx::components::RigidbodyComponent& rb);

    /**
     * @brief Emits rings from every Rigidbody (dynamic or kinematic) crossing a water surface:
     *  - a SPLASH when it enters the water moving down fast,
     *  - a WAKE trail while it moves through the water (relative to the current, so a crate
     *    drifting with a river leaves none), one ring per spacing travelled,
     *  - a BOB ring now and then while it heaves up and down in place.
     * Strength scales with speed and size. A body at rest -- asleep, or floating still -- emits
     * nothing, so a calm lake goes quiet.
     */
    void emit_ripples_(coopa::scene::Scene& scene);

    struct BuoyantEntry {
        Buoyancy* buoyancy = nullptr;
        coopa::physx::components::RigidbodyComponent* rigidbody = nullptr;
        float bound_radius = 0.0f;
    };

    static float shape_volume_(const coopa::physx::collision::Shape& s);

    /** @brief Element-wise |m|: maps a box's half extents to its rotated AABB half extents. */
    static glm::mat3 abs_mat3_(const glm::mat3& m) {
        return glm::mat3(glm::abs(m[0]), glm::abs(m[1]), glm::abs(m[2]));
    }

    /** @brief Builds Buoyancy::resolved from authored pontoons or the owner's colliders. */
    static void resolve_pontoons_(Buoyancy& b, int subdivision_cap);

    void gather_buoyant_(coopa::scene::Scene& scene);

    void substep_(coopa::physx::PhysicsWorld& world, float h);

    /**
     * @brief One floater's buoyancy for one substep: Archimedes and vertical drag per pontoon,
     *        horizontal drag toward the current through the centre of mass. Reads only shared
     *        const state and writes only `e`'s Body and Buoyancy -- safe to run concurrently with
     *        other floaters.
     * @return True if the floater was simulated in the water this substep (active_buoyant_count()).
     */
    bool floater_substep_(BuoyantEntry& e, coopa::physx::PhysicsWorld& world, const glm::vec3& gravity, float h,
                          const WaveQueryOptions& opt);

    /**
     * @brief A floater outside the simulation range. Asleep: nothing at all (it stays where it
     *        floated). Awake (knocked by something, or just left the range): cheap calm-water
     *        buoyancy -- one surface lookup under the centre of mass, no waves, no current --
     *        so it settles at its floating height instead of sinking, then is put to sleep.
     */
    void frozen_substep_(BuoyantEntry& e, coopa::physx::dynamics::Body& body, const glm::vec3& gravity, float h);

    coopa::job::JobEngine*          jobs_      = nullptr; ///< This frame's (FrameContext::jobs); may be null.
    coopa::gfx::core::Device*       device_    = nullptr;
    coopa::gfx::memory::Allocator*  allocator_ = nullptr;
    coopa::asset::AssetManager*     assets_    = nullptr;

    coopa::physx::system::PhysicsSystem* physics_ = nullptr;
    coopa::event::Connection substep_connection_;

    std::vector<WaterBody*> bodies_;
    std::vector<WaterBody*> active_bodies_; ///< Baked bodies within reach of the focus (per frame).
    std::vector<BuoyantEntry> buoyant_;
    std::vector<Ripple> ripples_;
    std::unordered_map<const coopa::physx::components::RigidbodyComponent*, RippleEmitter> emitters_;
    float       time_ = 0.0f;
    bool        simulating_ = false;   ///< Last execute()'s scene.is_simulating(), for render_time().
    std::size_t active_count_ = 0;

    WaterSettings settings_;
    glm::vec3     focus_{0.0f};
    bool          has_focus_ = false;
    bool          focus_override_ = false;
};

/**
 * @brief Constructs a WaterSystem and registers it just ahead of the physics phase.
 *        GPU arguments may be null for CPU-only (headless test) use.
 */
WaterSystem* install_water_system(coopa::scene::Scene& scene, coopa::gfx::core::Device* device = nullptr,
                                         coopa::gfx::memory::Allocator* allocator = nullptr,
                                         coopa::asset::AssetManager* assets = nullptr,
                                         int order = k_water_system_order);

} // namespace water
} // namespace toy

#endif // TOYENGINE_WATER_WATER_SYSTEM_H
