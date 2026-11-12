/**
 * @file mesh_renderer.h
 * @brief Component that references a GPU mesh for rendering.
 *
 * Holds a coopa::asset::AssetHandle to a coopa::gfx::engine::data::Mesh
 * (GPU-resident), obtained from register_render_components()'s
 * "MeshRenderer" parser after parsing the scene YAML's MeshRenderer
 * component. Multiple objects loading the same resolved mesh path share
 * one underlying asset slot (see coopa::asset::AssetManager); the handle
 * itself is a lightweight, refcounted, non-owning reference.
 */

#ifndef GFXCOOPA_ENGINE_COMPONENTS_MESH_RENDERER_H
#define GFXCOOPA_ENGINE_COMPONENTS_MESH_RENDERER_H

#include <coopa/scene/component.h>
#include <coopa/asset/asset_handle.h>
#include <glm/glm.hpp>
#include <string>
#include <utility>
#include <vector>
#include <memory>

#include <gfxcoopa/engine/data/mesh.h>
#include <gfxcoopa/engine/data/texture.h>

namespace coopa {
namespace gfx {
namespace engine {
namespace components {

/**
 * @brief How a material's alpha channel is interpreted by the renderer.
 */
enum class AlphaMode {
    Opaque, /**< Alpha ignored; the material is fully opaque. */
    Mask,   /**< Alpha-tested: discarded below alpha_cutoff, otherwise fully opaque. */
    Blend   /**< Alpha-blended by the forward transparent pass. */
};

/**
 * @brief How a toon material's specular highlight is shaped -- see PBRMaterial::toon_specular.
 *
 * Every mode keeps the Cook-Torrance lobe's own position and width (so the highlight still
 * grows with roughness and still takes its colour from F0/metallic); only its *edge* changes.
 */
enum class ToonSpecular {
    Pbr,     /**< Untouched smooth Cook-Torrance highlight. */
    Hard,    /**< One hard-edged step cut out of the GGX lobe. */
    TwoStep, /**< A hard bright core plus a dimmer outer step. */
    Off      /**< No specular at all -- diffuse bands only. */
};

/**
 * @struct PBRMaterial
 * @brief A mesh's surface parameters, as authored in scene YAML.
 *
 * Covers the Cook-Torrance inputs (albedo, metallic, roughness, emissive), the
 * alpha mode that decides which draw list the mesh joins, optional texture map
 * paths, and the name of a SurfaceShaderDesc variant when the material overrides
 * the stock pipeline.
 */
struct PBRMaterial {
    glm::vec3 albedo    = {0.8f, 0.8f, 0.8f};
    float     metallic  = 0.0f;
    float     roughness = 0.5f;
    float     ao        = 1.0f;

    float     alpha        = 1.0f;              /**< Straight (non-premultiplied) opacity. */
    AlphaMode alpha_mode   = AlphaMode::Opaque; /**< Selects the draw list this material joins. */
    float     alpha_cutoff = 0.5f;              /**< Only meaningful for AlphaMode::Mask. */

    /// Backface culling for this material, in the stock (no `shader:` override) G-buffer/Mask
    /// pipeline only. false renders both winding orders -- for a mesh that is not a closed solid, or
    /// where the CUTOUT/Mask alpha test intentionally exposes the interior (see
    /// assets/scenes/pixel_demo/scene.yaml's cutout_sphere.000). Ignored when `shader` is set
    /// (a named SurfaceShaderDesc's own `cull` always wins -- see GBufferPipeline::add_variant())
    /// and by the forward BLEND transparent pass (TransparentPass hardcodes CullMode::Back for
    /// every BLEND material -- see that pass's add_variant() doc). NOT consumed by
    /// SdfRenderer, same treatment as refraction/ior below -- parses and stores without effect.
    bool      cull_backfaces = true;

    /// Emissive radiance colour, linear space, before emissive_strength. Added to the
    /// shaded result *after* AO/SSAO attenuation -- an emissive surface glows even in a
    /// dark crevice. Not a real-time light source: no pass gathers emissive into direct
    /// lighting, so it reaches neighbouring surfaces only via the screen-space SSR/SSGI
    /// terms that sample lit scene colour (and, for baked probes, via GiBaker). Deferred
    /// (opaque G-buffer) path only -- the forward transparent/probe-capture paths don't
    /// carry this field.
    glm::vec3 emissive = {0.0f, 0.0f, 0.0f};

    /// Multiplier on `emissive`, kept separate so a scene can drive an HDR intensity (>1)
    /// without denormalising the authored colour. Only ever multiplied together on the
    /// way to the GPU -- see gpu_emissive().
    float     emissive_strength = 1.0f;

    /// Screen-space refraction (bent, blurred, absorption-tinted background sample) applied
    /// by the forward MESH transparent pass -- see toyengine's transparent.frag/refraction.glsl.
    /// Only meaningful when is_blended() is also true; a Mask/Opaque material with
    /// refraction=true is simply never drawn by the pass that reads it. NOT consumed by
    /// SdfRenderer/the SDF forward pass (sdf_forward.frag) even though PBRMaterial is shared
    /// with it -- these fields parse and store on an SdfRenderer's material without effect;
    /// see toyengine's refraction plan for why SDF glass was deliberately excluded.
    bool      refraction           = false;

    /// Negative (the default) means "not set by this material" -- the consumer (toyengine's
    /// record_transparent_()) falls back to its own engine-wide default (ToyRenderConfig::
    /// refraction_ior) in that case. A real IOR is never negative, so this sentinel can't
    /// collide with an authored value. Kept negative rather than defaulting to, say, 1.45
    /// directly so a scene author's choice and "the engine picked something" stay
    /// distinguishable -- changing the engine-wide default in config.yaml then actually
    /// changes every object that didn't override it, instead of only new ones.
    float     ior                  = -1.0f; /**< < 0 => use ToyRenderConfig::refraction_ior; else the IOR (glass ~1.45, water ~1.33). */
    float     refraction_thickness = -1.0f; /**< < 0 => use ToyRenderConfig::refraction_thickness; else world-space ray distance through the object. */
    glm::vec3 refraction_tint      = {-1.0f, -1.0f, -1.0f}; /**< Any component < 0 => use ToyRenderConfig::refraction_tint; else the Beer-Lambert absorption tint. */

    std::string texture_albedo             = "";
    std::string texture_normal             = "";
    std::string texture_metallic_roughness = "";

    /// Alpha-test mask for AlphaMode::Mask (CUTOUT), glTF/Unity convention: only the **alpha**
    /// channel is read, sampled at the mesh's UV and multiplied into albedo.a before the cutoff
    /// test (see gpu_alpha_cutoff()/has_alpha_mask()). Always linear, never sRGB -- it's a scalar
    /// coverage value, not color data. Ignored by SdfRenderer (SDFs have no UVs to sample with).
    std::string texture_alpha_mask         = "";

    /// Height map for TESSELLATED renderers (MeshRenderer::tessellation): the red channel,
    /// times displacement_scale, pushes each generated vertex out along its normal (Unity's
    /// tessellation + displacement). Read in the tessellation evaluation stage only; an
    /// untessellated draw ignores it. Linear, never sRGB.
    std::string texture_displacement       = "";
    float       displacement_scale         = 0.05f;   ///< Metres at a height-map value of 1.

    /// Weather snow cover lands on this material's up-facing, sky-open surfaces (the G-buffer
    /// backbone's snow layer, toyengine's gfx/surface/snow.glsl). False opts out -- emissive
    /// panels, interiors the occlusion map misses, anything snow must never whiten.
    bool        snow                       = true;

    /// Rain and local wet patches darken and gloss this material, and pool in its up-facing
    /// parts (the backbone's wet layer, toyengine's gfx/surface/wetness.glsl). Opt **in**,
    /// unlike `snow`: the stock weather conditions carry wetness (rain 0.8, storm 1.0,
    /// overcast 0.25), so defaulting it on would restyle every scene that sees rain.
    bool        wet                        = false;


    /// Set by `toy::fire::FireSystem` while this material is BURNING, which switches on the
    /// burn-front surface layer (`gfx/surface/burn.glsl`). Runtime state, not authoring: it is
    /// not read from YAML and not written back out, and a material nobody has set alight never
    /// pays for the layer.
    bool burning = false;

    // --- Toon (cel) shading ---------------------------------------------------------------
    // Layered ON TOP of this material's PBR inputs rather than replacing them: albedo,
    // metallic, roughness, normal maps, the indirect/ambient term, screen-space AO and SSR all
    // keep working exactly as they do for a PBR material, and the bands follow the real light
    // visibility the scene produces (N.L folded with the cascaded shadow map's PCSS penumbra,
    // the screen-space contact-shadow march and the cloud-shadow map). See
    // assets/shaders/gfx/toon.glsl for the shading math and gpu_toon_code() for the encoding.
    //
    // DEFERRED (opaque G-buffer) PATH ONLY, the same restriction `emissive` above documents:
    // the forward BLEND transparent pass and the probe-capture path don't carry these fields,
    // so a BLEND material with toon_* set shades as plain PBR.

    /// Number of discrete bands the light's visibility is quantized into. 0 or 1 (the default)
    /// leaves this material fully PBR, and then every other toon_* field below except
    /// toon_outline does nothing. Clamped to [2, 8] -- past that the steps stop reading as
    /// bands at all.
    int toon_bands = 0;

    /// Shape of the specular highlight's edge. Meaningless unless toon_bands > 1.
    ToonSpecular toon_specular = ToonSpecular::Pbr;

    /// Fresnel-shaped silhouette accent, per light: it appears on the side the light is on,
    /// takes that light's colour, and goes away in shadow. 0 disables it. Quantized to 0.25
    /// steps and clamped to 1.75. Unrelated to the engine-wide `rim_strength` render setting,
    /// which is a flat view-space accent applied to every surface.
    float toon_rim = 0.0f;

    /// How soft each band's edge is, as a share of one band's width: 0 is a hard cel
    /// terminator, 1 would smear a band into its neighbour. Snapped to one of
    /// {0.0, 0.15, 0.35, 0.70}. Edge antialiasing happens regardless of this value.
    float toon_softness = 0.0f;

    /// Draw the stylize pass's outline on this material. Needs the `outline_enabled` render
    /// setting on and `outline_mode: toon`; with `outline_mode: all` every surface is outlined
    /// and this flag is ignored. Independent of toon_bands, so a plain PBR material can be
    /// outlined too.
    bool toon_outline = false;

    /// Empty (the default) selects the stock surface shader for this material's
    /// alpha_mode. Otherwise the name of a shader registered
    /// in a pipeline::SurfaceShaderRegistry (see gfxcoopa/pipeline/surface_shader.h and the
    /// layered-shaders plan's gfx/surface/*.glsl backbones): a scene author writing
    /// `shader: foliage` picks the same G-buffer/shadow/shadow-cube backbone their
    /// alpha_mode already selects, but with foliage's displacement/shading hooks instead of
    /// the identity default. An unregistered name is a scene-load error (see
    /// SurfaceShaderRegistry::require()), never a silent fallback to stock.
    std::string shader = "";

    /// Four author-defined floats reaching the surface shader's gfx_params (see
    /// gfx/surface/gbuffer_vs.glsl) -- e.g. foliage's wind strength/frequency/direction.xy,
    /// or water's base Gerstner wave (amplitude, wavelength, direction, steepness). Meaningless (and unread by every
    /// backbone) when shader is empty. Zero-initialized rather than left uninitialized so a
    /// material that sets `shader` but forgets a param gets 0.0, not garbage.
    glm::vec4 shader_params = {0.0f, 0.0f, 0.0f, 0.0f};

    /// Eight more author-defined floats, as two vec4s, for derived shaders whose hooks need
    /// more than shader_params' four -- e.g. water's foam/shoreline look (see
    /// water_surface.glsl). Unlike shader_params, these reach the FRAGMENT stage of the forward
    /// transparent backbone only (gfx_params_ext0/1 in gfx/surface/transparent_fs.glsl): the
    /// opaque G-buffer/shadow backbones' push blocks have no room left under Vulkan's
    /// guaranteed 128 bytes, and nothing opaque needs them yet. Zero-initialized for the same
    /// reason as shader_params.
    glm::vec4 shader_params_ext[2] = {glm::vec4(0.0f), glm::vec4(0.0f)};

    // Populated by register_render_components()'s "MeshRenderer" parser once the corresponding
    // texture_* path above has been loaded via coopa::asset::AssetManager. All four handles are
    // consumed: engine::util::MaterialTextureCache binds each (or a neutral 1x1 fallback --
    // white for alpha_mask/albedo/metallic_roughness, flat-up for normal) into the material
    // descriptor set every textured pass (G-buffer, shadow, forward transparent, transparent
    // capture, probe capture) binds -- see has_albedo_map()/has_normal_map()/
    // has_metallic_roughness_map()/has_alpha_mask().
    coopa::asset::AssetHandle<coopa::gfx::engine::data::Texture> albedo_handle;
    coopa::asset::AssetHandle<coopa::gfx::engine::data::Texture> normal_handle;
    coopa::asset::AssetHandle<coopa::gfx::engine::data::Texture> metallic_roughness_handle;
    coopa::asset::AssetHandle<coopa::gfx::engine::data::Texture> alpha_mask_handle;
    coopa::asset::AssetHandle<coopa::gfx::engine::data::Texture> displacement_handle;

    /** @brief Returns true when this material must be drawn by the forward transparent pass. */
    bool is_blended() const { return alpha_mode == AlphaMode::Blend; }

    /** @brief Returns true when this material should be refracted by the forward MESH pass. */
    bool has_refraction() const { return is_blended() && refraction; }

    /**
     * @brief Returns true when this Mask material has a loaded alpha mask texture to sample.
     *
     * When false (Opaque/Blend, or a Mask material with no texture_alpha_mask), the G-buffer
     * and shadow passes bind a 1x1 white fallback instead, which collapses the shader-side test
     * back to the constant material.albedo.a test.
     */
    bool has_alpha_mask() const { return alpha_mode == AlphaMode::Mask && alpha_mask_handle.is_loaded(); }

    /**
     * @brief Returns true when this material has a loaded albedo (base color) map to sample.
     *
     * Unlike has_alpha_mask(), this does NOT gate on alpha_mode -- an albedo map modulates
     * the base color for Opaque/Mask/Blend materials alike, not just Mask ones.
     */
    bool has_albedo_map() const { return albedo_handle.is_loaded(); }

    /** @brief Returns true when this material has a loaded tangent-space normal map to sample. */
    bool has_normal_map() const { return normal_handle.is_loaded(); }

    /** @brief Returns true when this material has a loaded metallic/roughness map to sample. */
    bool has_metallic_roughness_map() const { return metallic_roughness_handle.is_loaded(); }

    /** @brief Returns true when this material has a loaded displacement (height) map. */
    bool has_displacement_map() const { return displacement_handle.is_loaded(); }

    /**
     * @brief Returns the alpha cutoff as the GPU shader sees it.
     *
     * A value of 0.0 disables the discard entirely, which is what OPAQUE and BLEND
     * materials need since only MASK performs an alpha test in the G-buffer pass. When
     * has_alpha_mask() is true, the shader multiplies this against the sampled mask's alpha
     * rather than the constant material.albedo.a alone -- see the caller's G-buffer and
     * shadow shaders (toyengine's gfx/surface/gbuffer_fs.glsl / shadow_fs.glsl).
     *
     * @return 0.0 for non-Mask materials, otherwise alpha_cutoff clamped to (0, 1].
     */
    float gpu_alpha_cutoff() const {
        return alpha_mode == AlphaMode::Mask ? glm::clamp(alpha_cutoff, 0.0001f, 1.0f) : 0.0f;
    }

    /** @brief Returns true when this material contributes any emissive radiance at all. */
    bool is_emissive() const {
        return emissive_strength > 0.0f && (emissive.r > 0.0f || emissive.g > 0.0f || emissive.b > 0.0f);
    }

    /**
     * @brief Returns the emissive term as the GPU shader sees it.
     *
     * Colour and strength collapse into one pre-multiplied vec4 for the same reason
     * gpu_alpha_cutoff() folds alpha_mode into its result: it keeps the shader side to a
     * single add, and it makes the batching predicates compare what actually reaches the
     * GPU, so {1,0,0} x 2.0 and {2,0,0} x 1.0 correctly merge into one batch. Negative
     * authored values are clamped away -- a negative emissive would subtract light from
     * the frame, which no pass expects.
     *
     * @return vec4(emissive * emissive_strength, 0.0); .w is reserved and currently unread.
     */
    glm::vec4 gpu_emissive() const {
        return glm::vec4(glm::max(emissive, glm::vec3(0.0f)) * glm::max(emissive_strength, 0.0f), 0.0f);
    }

    /** @brief Returns true when this material's diffuse is banded (cel-shaded). */
    bool is_toon() const { return toon_bands > 1; }

    /// The four band-edge softness values toon_softness snaps to. Mirrored by
    /// GFX_TOON_SOFTNESS in assets/shaders/gfx/toon.glsl, which decodes the index this
    /// table's position becomes -- the two must stay in step.
    static constexpr float kToonSoftness[4] = {0.0f, 0.15f, 0.35f, 0.70f};

    /**
     * @brief Returns the toon settings packed as the GPU shader sees them.
     *
     * The deferred lighting pass sees only the G-buffer, so these per-material settings
     * travel as one integer in the G-buffer's emissive alpha channel (a half float, which
     * represents integers up to 2048 exactly). This is the single place the packing --
     * and the quantization it imposes -- lives, for the same reason gpu_emissive() and
     * gpu_alpha_cutoff() exist: the batching predicates then compare what actually reaches
     * the GPU, so two materials differing only below a quantization step correctly merge
     * into one batch.
     *
     * | bits | field                                                     |
     * |------|-----------------------------------------------------------|
     * | 0-2  | bands: 0 = PBR shading, else 1..7 for 2..8 bands           |
     * | 3-4  | ToonSpecular                                              |
     * | 5-7  | toon_rim / 0.25, 0..7                                     |
     * | 8-9  | index into kToonSoftness                                  |
     * | 10   | toon_outline                                              |
     *
     * @return 0 for a material that is neither banded nor outlined, which is what every
     *         non-toon material writes and what the shader tests for to skip the toon path
     *         entirely.
     */
    uint32_t gpu_toon_code() const {
        // Outline is independent of banding: an un-banded PBR material may still be outlined.
        uint32_t code = toon_outline ? (1u << 10) : 0u;
        if (!is_toon()) return code;

        code |= static_cast<uint32_t>(glm::clamp(toon_bands, 2, 8) - 1);
        code |= (static_cast<uint32_t>(toon_specular) & 3u) << 3;
        code |= static_cast<uint32_t>(glm::round(glm::clamp(toon_rim, 0.0f, 1.75f) / 0.25f)) << 5;

        uint32_t soft = 0u;
        float    best = 1e9f;
        for (uint32_t i = 0; i < 4; ++i) {
            const float d = glm::abs(glm::clamp(toon_softness, 0.0f, 1.0f) - kToonSoftness[i]);
            if (d < best) { best = d; soft = i; }
        }
        code |= soft << 8;
        return code;
    }
};

/**
 * @class MeshRenderer
 * @brief References a GPU Mesh and marks the owning object as renderable.
 *
 * The mesh_path field stores the logical path from the scene YAML (e.g. "cube.000").
 * register_render_components()'s "MeshRenderer" parser resolves this to a full
 * filesystem path, loads the mesh YAML, creates a coopa::gfx::engine::data::Mesh,
 * and sets it here.
 */
class MeshRenderer : public coopa::scene::Component {
public:
    MeshRenderer() = default;

    std::string type_name() const override { return "MeshRenderer"; }

    /**
     * @brief Sets the logical mesh path (e.g. "cube.000").
     *
     * Called by the registered parser when parsing the YAML component data.
     *
     * @param path Logical mesh path relative to the scene's meshes/ directory.
     */
    void set_mesh_path(const std::string& path) { mesh_path_ = path; }

    /** @brief Returns the logical mesh path. */
    const std::string& mesh_path() const { return mesh_path_; }

    /**
     * @brief Sets the GPU mesh handle.
     *
     * Called once AssetManager::load()/load_async() has been kicked off for
     * this renderer's mesh_path. The handle may still be Loading at this
     * point (async) -- see is_ready().
     *
     * @param mesh Refcounted handle to the GPU-resident Mesh asset.
     */
    void set_mesh(coopa::asset::AssetHandle<coopa::gfx::engine::data::Mesh> mesh) {
        mesh_ = std::move(mesh);
    }

    /**
     * @brief Returns the mesh handle. Dereference only after checking is_ready() (or the handle's own is_loaded()).
     */
    const coopa::asset::AssetHandle<coopa::gfx::engine::data::Mesh>& get_mesh() const { return mesh_; }

    /**
     * @brief Returns true if this renderer has a valid GPU mesh ready to draw.
     */
    bool is_ready() const { return mesh_.is_loaded(); }

    /// Slot 0's material -- the whole mesh for a mesh without material slots.
    PBRMaterial material;

    /**
     * Materials for the mesh's other slots (submeshes, see data::MeshPart): entry i is slot
     * i + 1. A slot with no entry here falls back to `material`. Filled from YAML
     * `materials:` -- a list (by slot index, entry 0 = slot 0) or a map keyed by the mesh's
     * slot names, which can only be resolved once the mesh has loaded (resolve_slot_names()).
     */
    std::vector<PBRMaterial> slot_materials;

    /** @brief The material for slot `slot`. */
    const PBRMaterial& material_for(uint32_t slot) const {
        if (slot == 0 || slot > slot_materials.size()) return material;
        return slot_materials[slot - 1];
    }
    /** @brief Writable slot material (grows the table; slot 0 is `material`). */
    PBRMaterial& material_for_mut(uint32_t slot) {
        if (slot == 0) return material;
        while (slot_materials.size() < slot) slot_materials.push_back(material);
        return slot_materials[slot - 1];
    }

    /// Materials keyed by slot NAME, waiting for the mesh to load (see resolve_slot_names()).
    std::vector<std::pair<std::string, PBRMaterial>> pending_named_materials;

    /**
     * @brief Applies pending name-keyed materials once the mesh (and so its slot names) is
     *        ready. Cheap no-op otherwise; the render pipeline calls it every frame.
     */
    void resolve_slot_names() {
        if (pending_named_materials.empty() || !is_ready()) return;
        const auto& mesh = *mesh_.get();
        for (auto& [name, mat] : pending_named_materials) {
            const int slot = mesh.slot_index(name);
            if (slot < 0) continue;   // unknown slot name: ignored, as an unknown key would be
            material_for_mut(static_cast<uint32_t>(slot)) = mat;
        }
        pending_named_materials.clear();
    }

    /// Whether reflection probes may bake this renderer into their captured
    /// cubemaps. Defaults to true (static scenery). Set false on animated /
    /// otherwise-moving objects: probe bakes are static one-shot captures, so
    /// a moving object frozen into a cubemap goes stale the instant it moves
    /// -- and for an object reflecting itself, shows up as a visible
    /// mismatch (probe capture uses a simplified, non-recursive shading path
    /// that looks different from the main render). Matches standard engine
    /// practice (e.g. Unity's per-renderer "Reflection Probes" toggle):
    /// static geometry contributes real captured detail to reflections,
    /// dynamic objects rely on SSR alone.
    bool affects_reflection_probes = true;

    /// False keeps this renderer out of every shadow pass (it still receives shadows) -- grass,
    /// small clutter, anything whose shadow costs more than it shows.
    bool cast_shadows = true;

    /// > 0: cast shadows only while within this many metres of the main camera (bounds to
    /// eye). 0: always. A large surface split into pieces (terrain chunks) drops its distant
    /// pieces from the shadow passes this way.
    float shadow_max_distance = 0.0f;

    /// Multiplies this renderer's projected screen size before LOD selection: > 1 keeps
    /// detail longer (a hero prop), < 1 drops it sooner. The mesh's own LOD table (its
    /// `lods` block or `.lod.yaml` sidecar -- see data::Mesh::build_cpu) sets the levels.
    float lod_bias = 1.0f;

    /// False pins this renderer to LOD 0 and ignores the mesh's cull_screen_size.
    bool lods_enabled = true;

    /**
     * Camera-adaptive tessellation (Unity's "tessellation" geometry option): each triangle is
     * subdivided on the GPU so its edges are about `edge_pixels` render pixels long, up to
     * `max_factor` per edge, falling back to the plain mesh beyond `max_distance` metres. The
     * material's surface shader (its displacement hook -- water's waves, foliage, snow) runs on
     * the generated vertices, and a displacement map (PBRMaterial::texture_displacement) adds
     * detail. Needs a device with tessellation (the renderer draws untessellated, warning
     * once, otherwise). Applies to the G-buffer, both shadow passes and the forward
     * transparent pass, so displaced geometry casts matching shadows.
     */
    struct Tessellation {
        bool  enabled      = false;
        float edge_pixels  = 6.0f;    ///< Target edge length in render pixels (smaller = denser).
        float max_factor   = 16.0f;   ///< Cap on subdivisions per edge (the hardware allows 64).
        float max_distance = 60.0f;   ///< Beyond this the mesh draws as authored (factor 1).
    } tessellation;

    /// Runtime-only tessellation a system imposes (e.g. the water quality tier on water tiles);
    /// never serialized, so the authored `tessellation` stays what the scene file says. Used
    /// instead of `tessellation` while `has_tessellation_override` is set.
    Tessellation tessellation_override;
    bool has_tessellation_override = false;

    /** @brief The tessellation the renderer draws with: the override if set, else the authored block. */
    const Tessellation& effective_tessellation() const {
        return has_tessellation_override ? tessellation_override : tessellation;
    }

private:
    std::string mesh_path_; /**< Logical mesh path from YAML. */
    coopa::asset::AssetHandle<coopa::gfx::engine::data::Mesh> mesh_; /**< Refcounted GPU-resident mesh handle. */
};

} // namespace components
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // GFXCOOPA_ENGINE_COMPONENTS_MESH_RENDERER_H
