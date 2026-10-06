/**
 * @file particle_types.h
 * @brief The plain-data contract between toyengine/particles/ (which simulates) and the
 *        renderer (which draws): one GPU instance per particle, and the per-system draw
 *        batches Engine hands to PixelRenderPipeline::set_particle_state() each frame.
 *
 * glm only -- no Vulkan, no scene types beyond an opaque MeshRenderer pointer -- so the
 * particle module can fill these directly with no copy, and the render layer never includes
 * the particle module (the same split as WaterFrameState; see pixel_render_types.h).
 */

#ifndef TOYENGINE_RENDER_PARTICLE_TYPES_H
#define TOYENGINE_RENDER_PARTICLE_TYPES_H

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

namespace coopa { namespace gfx { namespace engine { namespace components {
class MeshRenderer;
struct PBRMaterial;
}}}}

namespace toy {
namespace render {

/// How a particle quad is oriented. Values are pushed to particle.vert as-is.
enum class ParticleRenderMode : uint32_t {
    Billboard  = 0,   ///< Faces the camera (the view plane).
    Stretched  = 1,   ///< Long axis along the velocity, widened toward the camera (sparks, rain).
    Horizontal = 2,   ///< Flat on the world XY plane (ground rings, decals).
    Vertical   = 3,   ///< Upright, turning only about world Z to face the camera (flames, grass cards).
    Aligned    = 4,   ///< In the plane of the particle's own orientation (a surface's normal).
    Mesh       = 5,   ///< An instanced mesh through the opaque G-buffer -- never a quad.
    None       = 6,   ///< Simulated, not drawn.
};

/// The procedural sprite a quad draws (or `Texture`: the system's albedo map / flipbook).
/// Values are read by particle.frag; keep the two in sync.
enum class ParticleSprite : uint32_t {
    Soft    = 0,   ///< Smooth radial glow.
    Circle  = 1,   ///< Hard-edged toon disc with a light rim.
    Puff    = 2,   ///< Lumpy cloud ball, lit as a sphere -- smoke, dust, steam.
    Flame   = 3,   ///< Teardrop tongue with a hot banded core and a licking noise edge.
    Spark   = 4,   ///< Thin bright streak; pair with `stretched`.
    Ring    = 5,   ///< Annulus -- shockwaves, ripples, magic circles.
    Star    = 6,   ///< Four-point twinkle -- fireflies, glints, magic.
    Leaf    = 7,   ///< Pointed leaf with a midrib -- falling leaves, petals.
    Texture = 8,   ///< The material's albedo texture (optionally a flipbook atlas).
};

/**
 * @struct ParticleInstance
 * @brief One particle as particle.vert reads it: five vec4s at instance rate (locations 0..4).
 *        Must match particle.vert's inputs exactly.
 */
struct ParticleInstance {
    glm::vec4 pos_size{0.0f};      ///< xyz world position, w size (world metres, quad height).
    glm::vec4 color{1.0f};         ///< Linear RGBA (straight alpha).
    glm::vec4 velocity_rot{0.0f};  ///< xyz world velocity (stretched), w in-plane rotation (rad).
    glm::vec4 orient{0.0f, 0.0f, 0.0f, 1.0f};  ///< Orientation quaternion (x, y, z, w) -- `aligned` mode's frame.
    glm::vec4 misc{0.0f};          ///< x normalized age, y per-particle random [0,1), z flipbook frame, w unused.
};
static_assert(sizeof(ParticleInstance) == 80, "ParticleInstance must match particle.vert's 5 x vec4 layout");

/**
 * @struct ParticleLook
 * @brief The per-system shading knobs, pushed once per batch. Field meanings mirror the
 *        ParticleSystem renderer settings (see toyengine/particles/particle_system.h).
 */
struct ParticleLook {
    ParticleRenderMode mode   = ParticleRenderMode::Billboard;
    ParticleSprite     sprite = ParticleSprite::Soft;
    float lit            = 0.0f;   ///< 0 unlit (emissive fire) .. 1 fully lit by sun/sky/point lights.
    float toon_bands     = 0.0f;   ///< > 1: lighting and flame / spark cores quantized into this many steps.
    float emissive       = 1.0f;   ///< HDR multiplier on colour (bloom picks up > 1).
    float additive       = 0.0f;   ///< 0 alpha blend .. 1 additive (premultiplied blend covers both).
    float softness       = 0.5f;   ///< Sprite edge: 0 crisp cel edge .. 1 fully feathered.
    float soft_distance  = 0.4f;   ///< Fade over this depth gap to opaque geometry (m); 0 off.
    float camera_fade    = 0.3f;   ///< Fade when closer than this to the camera (m); 0 off.
    float aspect         = 1.0f;   ///< Quad width / height.
    float stretch_speed  = 0.05f;  ///< Stretched: extra length per m/s of speed (s).
    float stretch_length = 1.0f;   ///< Stretched: multiplier on the base length.
    float distortion     = 0.5f;   ///< Noise breakup of the sprite edge (puff / flame / leaf).
    float opacity        = 1.0f;   ///< Final alpha multiplier.
    float pivot_z        = 0.0f;   ///< Shift the quad along its up axis, in sizes (0.5: base at the particle).
    glm::vec2 flipbook{1.0f, 1.0f};///< Atlas columns / rows for `Texture`.
};

/**
 * @struct ParticleDrawBatch
 * @brief One system's quads this frame, already in back-to-front order when the system sorts.
 *        `instances` points into the system's own buffer, valid until its next simulation step
 *        (Engine hands batches over after the scene update and before render()).
 */
struct ParticleDrawBatch {
    const ParticleInstance* instances = nullptr;
    uint32_t  count = 0;
    glm::vec3 sort_center{0.0f};   ///< Where the batch sorts among other transparent geometry.
    glm::vec3 bounds_min{0.0f};
    glm::vec3 bounds_max{0.0f};
    ParticleLook look;
    /// Its albedo (and flipbook) texture comes from this material's albedo slot; null or
    /// untextured draws the procedural `sprite` (the material cache binds white).
    const coopa::gfx::engine::components::PBRMaterial* texture_material = nullptr;
};

/**
 * @struct ParticleMeshBatch
 * @brief One mesh-mode system: `count` world matrices for one mesh + material, drawn through the
 *        opaque G-buffer and shadow passes as a single instanced batch. `proxy` is a MeshRenderer
 *        the system owns (never attached to the scene) carrying the mesh handle and material.
 */
struct ParticleMeshBatch {
    coopa::gfx::engine::components::MeshRenderer* proxy = nullptr;
    const glm::mat4* matrices = nullptr;
    uint32_t  count = 0;
    glm::vec3 bounds_min{0.0f};
    glm::vec3 bounds_max{0.0f};
};

/** @brief Everything the particle module hands the renderer for one frame. */
struct ParticleFrameState {
    std::vector<ParticleDrawBatch> quads;
    std::vector<ParticleMeshBatch> meshes;
    uint32_t total_quads() const {
        uint32_t n = 0;
        for (const auto& b : quads) n += b.count;
        return n;
    }
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PARTICLE_TYPES_H
