/**
 * @file hair_system.h
 * @brief `HairSystem` (order 64): grows every `Hair` component's groom and keeps the object that
 *        draws it in step.
 *
 * Order 64 -- the first free slot after the procedural graph (63) and well before Physics (100),
 * so a groom's guide strands are registered with the solver the same frame they appear.
 *
 * It runs in EDIT MODE too. A groom is authored data, not gameplay: dragging `density` in the
 * inspector has to show the result, exactly as terrain meshing keeps running in a held scene.
 *
 * ### What it owns, and why none of it lives on the component
 *
 * The editor destroys and recreates a component on every inspector keystroke, so everything
 * derived -- the strand set, the built mesh, the runtime child -- is filed here under
 * `Hair::state_key()` (the owner's NAME). Nothing is keyed by a `Component*` or a `SceneObject*`:
 * a published root outliving the component that made it is exactly how procgraph once produced a
 * use-after-free.
 *
 * ### Regeneration
 *
 * Re-scattering thousands of strands every frame while a slider moves would be unusable, so a
 * rebuild happens only when something that changes the geometry changes: `HairParams::hash()`
 * moves, the scalp mesh file changes on disk, or a rebuild is asked for. `regenerate: on_load`
 * builds once during warm-up -- the system participates in `warm_up_state()` so a loading screen
 * holds until the grooms are there, as terrain and water do.
 *
 * ### Why the scalp's triangles are cached HERE and not in the AssetManager
 *
 * Hair has to READ the scalp's geometry, but a loaded `data::Mesh` keeps no CPU vertex copy (its
 * staging buffer is released after the upload). The obvious fix -- a second, CPU-only asset type
 * loaded from the same mesh file -- cannot work: `AssetManager` keys a slot by the file's RESOLVED
 * PATH, not by (path, type), so asking for `meshes/head.yaml` as anything other than the
 * `data::Mesh` it is already loaded as is refused outright ("already loaded as a different asset
 * type"). That is the same one-path-one-type rule that stops a static mesh also loading as a
 * skinned one.
 *
 * So this system parses the mesh file itself and caches the result under the resolved path,
 * keyed on the file's write time so an edited scalp re-grows its hair. One parse per unique scalp
 * mesh, on the main thread, at the moment a groom is first built.
 *
 * ### The draw
 *
 * Geometry is published as a `RuntimeObject`-marked child of the scalp carrying its own
 * `MeshRenderer`, built in the SCALP'S LOCAL space with an identity child transform -- so the
 * scalp's world matrix applies exactly once -- and drawn by the ordinary mesh path. Shadows,
 * culling, LOD and the surface shaders therefore need no new render code.
 */

#ifndef TOYENGINE_HAIR_HAIR_SYSTEM_H
#define TOYENGINE_HAIR_HAIR_SYSTEM_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <coopa/debug/logger.h>
#include <coopa/scene/scene_system.h>
#include <gfxcoopa/engine/data/mesh.h>

#include <physxcoopa/hair/strand.h>

#include "toyengine/hair/hair.h"
#include "toyengine/hair/hair_data.h"

namespace coopa {
namespace physx { class PhysicsWorld; }
namespace asset { class AssetManager; }
namespace gfx {
namespace core { class Device; }
namespace memory { class Allocator; }
} // namespace gfx
namespace scene { class SceneObject; }
} // namespace coopa

namespace toy {
namespace hair {

/// @brief After procgraph (63), before Physics (100). See this file's doc.
constexpr int k_hair_system_order = 64;

/**
 * @class HairSystem
 * @brief Generates and publishes every groom in the scene.
 */
class HairSystem : public coopa::scene::ISceneSystem {
public:
    /**
     * @param device            For the groom's GPU vertex/index buffers.
     * @param allocator         Likewise.
     * @param assets            Resolves asset paths and publishes the built mesh.
     * @param frames_in_flight  Vertex buffers a SIMULATED groom keeps, so a rewrite never races
     *                          the GPU still reading the previous frame. A static groom keeps one.
     */
    HairSystem(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
               coopa::asset::AssetManager& assets, std::uint32_t frames_in_flight);
    ~HairSystem() override;

    const char* system_name() const override { return "Hair"; }
    void        execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;
    /// @brief True: a groom is authored data the editor must see, not gameplay.
    bool        runs_in_edit_mode() const override { return true; }
    coopa::scene::WarmupState warm_up_state() const override;

    /// @brief Rebuilds the groom on the object named `key` on the next pass. What the inspector's
    ///        button and `regenerate: manual` use.
    void regenerate(const std::string& key);

    /// @brief Rebuilds every groom on the next pass.
    void regenerate_all();

    /// @brief Strands in the groom on `key`, or 0 if it has not been built. For the inspector and
    ///        the tests -- the one number that says whether a groom actually grew.
    std::size_t strand_count(const std::string& key) const;

    /// @brief Vertices in that groom's mesh, or 0. Lets a test prove geometry reached the GPU.
    std::size_t vertex_count(const std::string& key) const;

    /// @brief Guide strands actually being solved for `key`, or 0 when it is not simulated.
    std::size_t simulated_guide_count(const std::string& key) const;

    /**
     * @brief Rewrites every simulated groom's vertex buffer from the solver's particles.
     *
     * Called from `Engine::upload_dynamic_meshes_()` -- after Physics and TransformResolve, and
     * before the frame's command buffer is recorded. NOT from `Component::late_update()`, which
     * has no way to know the in-flight frame slot. Exactly `ClothRenderer::upload()`'s contract.
     *
     * @param scene      The live scene, for reaching the PhysicsSystem that owns the guides.
     * @param frame_slot The frame-in-flight slot to write; `Context::current_frame()`.
     */
    void upload(coopa::scene::Scene& scene, std::uint32_t frame_slot);

private:
    /// Everything derived for one groom. Keyed by owner name, never by a pointer.
    struct Groom {
        std::uint64_t params_hash = 0;
        std::string   scalp_key;             ///< Logical mesh key the strands were grown from.
        std::string   scalp_path;            ///< Its resolved file path, the scalp cache's key.
        std::filesystem::file_time_type scalp_write_time{};  ///< The file's stamp at build time.
        std::uint64_t guides_hash = 0;       ///< Content hash of the guide curves at build time.
        HairParams    params;                ///< What the geometry was actually built with.
        StrandSet     set;
        /// Kept only for the simulated path, which rewrites strands in place each frame.
        std::vector<Vertex> vertices;
        std::shared_ptr<coopa::gfx::engine::data::Mesh> mesh;

        // --- Simulated grooms only -------------------------------------------------------
        /// The guide strands this groom's render strands follow, owned by PhysicsWorld.
        coopa::physx::hair::StrandSetId strands;
        /// Which strand index each render strand follows, and by how much. `guide_weights` is
        /// parallel to `guide_index`, k entries per render strand.
        std::vector<std::uint32_t> guide_index;
        std::vector<float>         guide_weights;
        std::uint32_t              guides_per_strand = 0;
        /// Which render strand each GUIDE is, in guide order. Keeps the two in step without
        /// re-deriving the stride, which would drift if the strand count ever changed.
        std::vector<std::uint32_t> guide_strand;
        std::size_t                simulated_guides = 0;
        /// The scalp's inverse world matrix, refreshed each frame: the solver works in world
        /// space and the mesh is built in the scalp's local space.
        glm::mat4                  to_local{1.0f};
        /// Each render strand's points in the guide's RELATIVE frame at bind time, so following
        /// a guide reproduces the strand's own shape rather than snapping it onto the guide.
        std::vector<glm::vec3>     bind_offsets;

        bool built = false;
        bool pending = false;                ///< Could not read its scalp yet; holds warm-up open.
    };

    /// One parsed scalp mesh, shared by every groom grown from the same file.
    struct ScalpEntry {
        std::shared_ptr<coopa::gfx::engine::data::MeshCpuData> data;
        std::filesystem::file_time_type write_time{};
    };

    /// Parses (or returns the cached) triangles of `mesh_key`. Null when the file is unreadable.
    const ScalpEntry* load_scalp_(const std::string& mesh_key, const std::string& base_dir,
                                  const std::string& groom_key);
    /**
     * Snapshots every `Curve` under the object `hair.params.guides` names, into the SCALP'S local
     * space. Main thread, like procgraph's own curve binding, and for the same reason: the shape
     * is copied out rather than read live.
     */
    std::vector<HairGuide> collect_guides_(const Hair& hair, int points) const;
    /// Content hash of a guide set, so `on_change` re-grows when a drawn curve moves.
    static std::uint64_t hash_guides_(const std::vector<HairGuide>& guides);
    bool should_rebuild_(const Groom& groom, Hair& hair, const ScalpEntry& scalp,
                         std::uint64_t guides_hash, bool forced) const;
    void build_(const std::string& key, Groom& groom, Hair& hair, const ScalpEntry& scalp,
                std::vector<HairGuide> guides, std::uint64_t guides_hash,
                coopa::scene::Scene& scene);
    /// The half of build_() that does not care where the triangles came from -- a parsed mesh
    /// file for a rigid scalp, or a SkinnedMeshRenderer's bind pose for a skinned one.
    void build_on_(const std::string& key, Groom& groom, Hair& hair, const ScalpMesh& scalp,
                   std::vector<HairGuide> guides, std::uint64_t guides_hash,
                   coopa::scene::Scene& scene);
    void publish_(const std::string& key, Groom& groom, Hair& hair, coopa::scene::Scene& scene);
    /**
     * Re-attaches a groom's runtime child when it has gone missing without the groom changing.
     *
     * The editor destroys and recreates an object on every inspector keystroke, and that rebuild
     * lands AFTER this system has run for the frame -- taking the published child with it. The
     * groom's own state still says `built`, so nothing would ever mark it dirty again and the
     * hair would stay invisible until the scene was reloaded.
     */
    void republish_if_missing_(const std::string& key, Groom& groom, Hair& hair,
                               coopa::scene::Scene& scene);
    void refresh_material_(Groom& groom, Hair& hair) const;
    /// Finds the scene's PhysicsSystem by name, as the vehicle system does. Null before it binds.
    coopa::physx::PhysicsWorld* physics_world_(coopa::scene::Scene& scene) const;
    /// Registers (or re-registers) a simulated groom's guides with the physics world.
    void bind_strands_(Groom& groom, Hair& hair, coopa::scene::Scene& scene);
    /// Releases them. Called when a groom is rebuilt, stops simulating, or goes away.
    void release_strands_(Groom& groom, coopa::scene::Scene& scene);
    /// Pushes this frame's root transforms into the solver, before Physics steps.
    void drive_strand_roots_(Groom& groom, Hair& hair, coopa::scene::Scene& scene);
    /// The groom's runtime child, found by walking the owner's children -- never a cached pointer.
    coopa::scene::SceneObject* find_child_(coopa::scene::SceneObject& owner) const;
    void prune_(coopa::scene::Scene& scene, const std::unordered_set<std::string>& live);

    coopa::gfx::core::Device&       device_;
    coopa::gfx::memory::Allocator&  allocator_;
    coopa::asset::AssetManager&     assets_;
    std::uint32_t                   frames_in_flight_ = 1;

    std::unordered_map<std::string, Groom>      grooms_;
    std::unordered_map<std::string, ScalpEntry> scalps_;   ///< Keyed by resolved file path.
    std::unordered_set<std::string>        forced_;
    bool                                   force_all_ = false;
    /// Keys already complained about, so a bad scalp path warns once and not every frame.
    std::unordered_set<std::string>        warned_;
    coopa::debug::Logger                   logger_{"hair"};
};

/// @brief Installs HairSystem at k_hair_system_order. Call once per scene.
void install_hair_system(coopa::scene::Scene& scene, coopa::gfx::core::Device& device,
                         coopa::gfx::memory::Allocator& allocator, coopa::asset::AssetManager& assets,
                         std::uint32_t frames_in_flight);

} // namespace hair
} // namespace toy

#endif // TOYENGINE_HAIR_HAIR_SYSTEM_H
