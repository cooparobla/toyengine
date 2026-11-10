/**
 * @file surface_shader.h
 * @brief Registry of derived surface shaders a scene's materials may select by name.
 */

#ifndef COOPA_GFX_PIPELINE_SURFACE_SHADER_H
#define COOPA_GFX_PIPELINE_SURFACE_SHADER_H

#include <stdexcept>
#include <string>
#include <vector>

#include <gfxcoopa/types/enums.h>

namespace coopa {
namespace gfx {
namespace pipeline {

/**
 * @brief Which family of passes a SurfaceShaderDesc's entry points apply to.
 *
 * Mirrors PBRMaterial::AlphaMode's draw-list split: Opaque covers the G-buffer, directional
 * shadow, and cube shadow passes (materials with AlphaMode::Opaque or AlphaMode::Mask);
 * Transparent covers the forward transparent and transparent-capture passes
 * (AlphaMode::Blend). A single named shader belongs to exactly one domain -- a water
 * material is Blend/Transparent, a foliage material is Mask/Opaque, and neither needs the
 * other domain's entry points.
 */
enum class SurfaceShaderDomain {
    Opaque,
    Transparent,
};

/**
 * @struct SurfaceShaderDesc
 * @brief One derived shader: a name plus the logical entry-point names its family of
 *        passes should resolve instead of the stock ones.
 *
 * Every field below is a *logical* shader name (e.g. "foliage.vert", no ".spv", no
 * directory) exactly like the strings already passed to ToyRenderConfig::shaders() --
 * the caller resolves each one through its own ShaderLibrary before handing the resolved
 * .spv path to a pass's add_variant(). An empty field means "this shader doesn't override
 * that entry point" -- for the shadow entry points that's a real gap, not a convenience:
 * see gfx/surface/shadow_vs.glsl's file doc on why a caster whose shadow doesn't move with
 * its mesh is a visible bug, so this struct intentionally makes every entry point an
 * explicit opt-in rather than silently reusing the stock shadow shader for a mesh that
 * displaces itself.
 */
struct SurfaceShaderDesc {
    std::string name; ///< Referenced by PBRMaterial::shader; must be non-empty and unique.
    SurfaceShaderDomain domain = SurfaceShaderDomain::Opaque;

    std::string vert;              ///< G-buffer / transparent vertex entry point.
    std::string frag;              ///< G-buffer / transparent fragment entry point.
    std::string shadow_vert;       ///< Directional shadow vertex entry point (Opaque domain only).
    std::string shadow_frag;       ///< Directional shadow fragment entry point (Opaque domain only).
    std::string shadow_cube_vert;  ///< Point-light cube shadow vertex entry point (Opaque domain only).
    std::string shadow_cube_frag;  ///< Point-light cube shadow fragment entry point (Opaque domain only).

    /// Per-shader rasterization override -- e.g. foliage cards want CullMode::None (drawn
    /// from both sides) where the stock backbone defaults to CullMode::Back.
    coopa::gfx::CullMode cull = coopa::gfx::CullMode::Back;

    // --- Tessellation (optional; appended so positional initializers above stay valid) ---
    //
    // A tessellated draw runs a pass-through vertex stage, a control stage (edge factors from
    // camera distance) and an EVALUATION stage, which is where the shader's displacement
    // hook (gfx_surface_vertex) runs on the generated vertices. A shader with a stock vertex
    // stage (empty `vert`) uses the stock evaluation stages; one that overrides `vert` must
    // name its own `tese` (its hook compiled against the evaluation backbone), or it simply
    // draws untessellated. `tesc` overrides the control stage (e.g. a wider cull margin for
    // large displacement); empty uses the stock one.
    std::string tesc;              ///< G-buffer / transparent tessellation control (optional).
    std::string tese;              ///< G-buffer / transparent tessellation evaluation.
    std::string shadow_tese;       ///< Directional shadow tessellation evaluation (Opaque domain).
    std::string shadow_cube_tese;  ///< Cube shadow tessellation evaluation (Opaque domain).
    /// Shadow tessellation control overrides (optional; empty = the stock control stages).
    /// Needed when the edge factors must come from displaced endpoints the stock stage cannot
    /// see -- the camera and the shadow passes must agree on how each caster is subdivided.
    std::string shadow_tesc;       ///< Directional shadow tessellation control.
    std::string shadow_cube_tesc;  ///< Cube shadow tessellation control.
};

/**
 * @class SurfaceShaderRegistry
 * @brief Name -> SurfaceShaderDesc lookup, populated once at startup (alongside the
 *        ShaderLibrary it complements) and consulted by every pass that owns a
 *        per-shader pipeline variant map.
 *
 * Registration failures are startup errors on purpose: an unresolvable or duplicate shader
 * name is far cheaper to catch at scene-load time than at the first frame that tries to
 * draw with it.
 */
class SurfaceShaderRegistry {
public:
    /**
     * @brief Registers a derived shader.
     * @throws std::runtime_error if `desc.name` is empty or already registered.
     */
    void add(SurfaceShaderDesc desc) {
        if (desc.name.empty()) {
            throw std::runtime_error("[gfxcoopa] SurfaceShaderRegistry::add: shader name must not be empty");
        }
        if (find(desc.name) != nullptr) {
            throw std::runtime_error("[gfxcoopa] SurfaceShaderRegistry::add: duplicate shader name '" +
                                     desc.name + "'");
        }
        shaders_.push_back(std::move(desc));
    }

    /**
     * @brief Looks up a registered shader by name.
     * @param name Empty means "the stock shader for this material's AlphaMode".
     * @return The matching desc, or nullptr if `name` is empty or unregistered.
     */
    const SurfaceShaderDesc* find(const std::string& name) const {
        if (name.empty()) return nullptr;
        for (const auto& s : shaders_) {
            if (s.name == name) return &s;
        }
        return nullptr;
    }

    /**
     * @brief Validates that a material's shader reference resolves.
     * @throws std::runtime_error if `name` is non-empty and unregistered.
     */
    void require(const std::string& name) const {
        if (!name.empty() && find(name) == nullptr) {
            throw std::runtime_error("[gfxcoopa] SurfaceShaderRegistry: material references "
                                     "unregistered shader '" + name + "'");
        }
    }

    /** @brief Every registered shader, in registration order -- pass constructors iterate this. */
    const std::vector<SurfaceShaderDesc>& all() const { return shaders_; }

private:
    std::vector<SurfaceShaderDesc> shaders_;
};

} // namespace pipeline
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_PIPELINE_SURFACE_SHADER_H
