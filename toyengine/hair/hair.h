/**
 * @file hair.h
 * @brief The `Hair` component: a groom grown from the faces of the mesh beside it.
 *
 * A companion to a `MeshRenderer` (or a `SkinnedMeshRenderer`) on the SAME object -- that mesh is
 * the scalp. The component is authoring data only; everything derived -- the strands, the built
 * mesh, the object that draws it -- is owned by `HairSystem` and keyed by `state_key()`, because
 * the editor destroys and recreates a component on every inspector keystroke.
 *
 * The generated geometry is published as a `RuntimeObject`-marked child of the owner, so the
 * editor groups it under "Runtime", locks it, and never writes thousands of strands into the
 * scene file.
 *
 * @see hair_params.h for every authored key, hair_data.h for how a groom is grown.
 */

#ifndef TOYENGINE_HAIR_HAIR_H
#define TOYENGINE_HAIR_HAIR_H

#include <string>

#include <glm/glm.hpp>

#include <coopa/scene/component.h>
#include <gfxcoopa/engine/components/mesh_renderer.h>

#include "toyengine/hair/hair_params.h"

namespace coopa {
namespace asset {
class AssetManager;
}
} // namespace coopa

namespace toy {
namespace hair {

/**
 * @class Hair
 * @brief Hair or fur growing from the sibling renderer's mesh.
 */
class Hair : public coopa::scene::Component {
public:
    std::string type_name() const override { return "Hair"; }

    /// @brief The groom itself.
    HairParams params;

    /// @brief How the strands are shaded. Defaults to the `hair` surface shader, alpha-tested and
    ///        two-sided, which is what a ribbon groom needs; a `tube` groom may be fully opaque.
    coopa::gfx::engine::components::PBRMaterial material;

    /// @brief Mesh to grow from, as a `MeshRenderer`-style logical key. Empty (the default) takes
    ///        the sibling renderer's, which is what makes this a companion component.
    std::string scalp_mesh;

    /// @brief Live wind in m/s, written every frame by `toy::weather::WindSystem` -- pushed in,
    ///        never pulled, the same way a cloth's is. Not authored.
    glm::vec3 wind{0.0f};

    /// @brief How much of the weather's wind reaches this groom. Hair indoors wants 0.
    float weather_wind = 1.0f;

    /// @brief Where relative asset paths resolve from -- the loading scene's directory. Captured
    ///        at parse time because the system, which does the loading, has no `ParseContext`.
    std::string asset_base_dir;

    /// @brief Asks for a rebuild on the next system pass, whatever `params.regenerate` says.
    ///        What `regenerate: manual` and the inspector's button use.
    void request_regenerate() { regenerate_requested_ = true; }

    /// @brief Reads and clears the request. Called by HairSystem.
    bool take_regenerate_request() {
        const bool requested = regenerate_requested_;
        regenerate_requested_ = false;
        return requested;
    }

    /**
     * @brief The key HairSystem files this groom's derived state under.
     *
     * The owner's NAME, not its address: a component -- and the object under the editor -- can be
     * destroyed and recreated between frames, so anything meant to outlive one frame must be keyed
     * by something that survives that. The same reason `ProceduralGraph::state_key()` exists.
     */
    std::string state_key() const;

    /// @brief Name of the runtime child that draws this groom, under the owner.
    static const char* child_object_name() { return "HairMesh"; }

private:
    bool regenerate_requested_ = false;
};

/**
 * @brief Registers the "Hair" scene parser. Called once at startup.
 *
 * Takes the asset manager because `material:` may name a shared material asset and its textures,
 * which resolve exactly as any other renderer's do.
 */
void register_hair_components(coopa::asset::AssetManager& assets);

} // namespace hair
} // namespace toy

#endif // TOYENGINE_HAIR_HAIR_H
