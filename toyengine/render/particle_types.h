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
#include <memory>
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
    // --- Light and TAA (see particle.frag) ---
    bool  receive_shadows = true;  ///< Lit particles darken in the sun's / shadowed local lights' shadows.
    float scatter        = 0.0f;   ///< Forward scattering of the sun and point / spot lights toward the eye
                                   ///< (rain glinting against a lamp or the low sun); 0 off.
    float scatter_anisotropy = 0.75f; ///< Henyey-Greenstein g of that scattering: 0 even .. ~0.9 a tight forward lobe.
    float reactive       = 0.0f;   ///< TAA: how much it cuts the temporal history under it (0..1) -- fast,
                                   ///< thin particles (rain, snow, sparks) that would otherwise smear.
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
    /// Non-zero: a GPU-simulated system (`simulation: gpu`). `instances` is unused; the quads are
    /// GpuParticlePass's buffer for this id, drawn with draw_indirect(), and `count` is only the
    /// last read-back alive count (an estimate, >= 1) for stats and sorting.
    uint64_t gpu_id = 0;
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

/**
 * @struct GpuParticleParams
 * @brief One GPU-simulated system's per-frame parameters, uploaded whole into its params buffer.
 *        std430 layout, every member a vec4 / mat4: must match `Params` in
 *        assets/shaders/particles_gpu_common.glsl exactly.
 */
struct GpuParticleParams {
    glm::mat4 world{1.0f};        ///< Emitter now (spawn space -> world).
    glm::mat4 prev_world{1.0f};   ///< Emitter at the previous job (sub-frame spawn positions).
    glm::mat4 to_world{1.0f};     ///< Simulation space -> world (local: the emitter, else identity).
    glm::vec4 frame{0.0f};        ///< x dt, y noise time, z spawn count, w spawn seed (uint bits).
    glm::vec4 config{0.0f};       ///< x capacity, y sort mode (0 none, 1 distance, 2 oldest, 3 youngest), z local, w world scale.
    glm::vec4 eye{0.0f};          ///< xyz camera position, w flipbook frames.
    glm::vec4 shape0{0.0f};       ///< x shape type (EmitShape), y radius, z radius thickness, w cone angle (deg).
    glm::vec4 shape1{0.0f};       ///< x arc (deg), y random direction, z mesh normal offset, w edge length.
    glm::vec4 shape_box{0.0f};    ///< xyz box extents, w mesh triangle count.
    glm::vec4 shape_offset{0.0f}; ///< xyz offset, w mesh total area.
    glm::vec4 life_speed{0.0f};   ///< xy lifetime range, zw start speed range.
    glm::vec4 size_rot{0.0f};     ///< xy start size range, zw start rotation range (rad).
    glm::vec4 spin{0.0f};         ///< xy angular velocity range (rad/s), z align to normal, w random spin.
    glm::vec4 color_a{1.0f};
    glm::vec4 color_b{1.0f};
    glm::vec4 accel{0.0f};        ///< xyz gravity + force (simulation space), w drag.
    glm::vec4 velocity{0.0f};     ///< xyz constant velocity, w orbital (rad/s).
    glm::vec4 center{0.0f};       ///< xyz orbit centre (simulation space), w radial (m/s).
    glm::vec4 axis{0.0f};         ///< xyz orbit axis, w tumble (rad/s).
    glm::vec4 collision{0.0f};    ///< x collide, y ground height, z bounce, w horizontal speed kept.
    glm::vec4 wrap{0.0f};         ///< xyz wrap box (world), w wrap fade fraction (0 off).
    glm::vec4 noise{0.0f};        ///< x strength, y kill on collide, z flipbook mode, w flipbook fps.
    glm::vec4 misc{0.0f};         ///< x flipbook cycles, yzw inherited emitter velocity (world m/s).
    glm::vec4 emitter_rot{0.0f, 0.0f, 0.0f, 1.0f}; ///< Emitter rotation quaternion (xyzw).
    glm::vec4 noise_k[6]{};       ///< Turbulence waves: xyz wave vector, w omega.
    glm::vec4 noise_c[6]{};       ///< xyz curl amplitude (normalized k x a), w phase.
    glm::vec4 color_lut[64]{};    ///< colour_over_life, baked (64 samples over t in [0, 1]).
    glm::vec4 curve_lut[64]{};    ///< x size_over_life, y alpha_over_life, baked.
};
static_assert(sizeof(GpuParticleParams) == 3 * 64 + 21 * 16 + 12 * 16 + 128 * 16, "GpuParticleParams must stay vec4-packed (std430)");

/**
 * @struct GpuParticleJob
 * @brief One GPU-simulated system's work this frame: GpuParticlePass simulates every job (seen
 *        or not), and draws the ones that also have a ParticleDrawBatch with the same id.
 */
struct GpuParticleJob {
    uint64_t id = 0;              ///< Stable per ParticleSystem.
    uint32_t generation = 0;      ///< Bumped by clear()/restart(): the GPU state resets.
    uint32_t capacity = 0;        ///< max_particles: the buffers' fixed size.
    bool     sort = false;        ///< Bitonic sort (blended systems); additive ones skip it.
    GpuParticleParams params;
    /// Mesh emitter triangles (8 vec4 per triangle, see MeshSurface::export_faces()); null: none.
    std::shared_ptr<const std::vector<glm::vec4>> triangles;
};

/** @brief Everything the particle module hands the renderer for one frame. */
struct ParticleFrameState {
    std::vector<ParticleDrawBatch> quads;
    std::vector<ParticleMeshBatch> meshes;
    std::vector<GpuParticleJob>    gpu;     ///< `simulation: gpu` systems, simulated on the GPU.
    uint32_t total_quads() const {
        uint32_t n = 0;
        for (const auto& b : quads) n += b.count;
        return n;
    }
};

} // namespace render
} // namespace toy

#endif // TOYENGINE_RENDER_PARTICLE_TYPES_H
