/**
 * @file particle_system.h
 * @brief `ParticleSystem` -- the scene component: a Unity-style module stack (main, emission,
 *        shape, velocity, forces, noise, colour / size / rotation over life, collision,
 *        renderer) with Blender's mesh emitter and hair-style scatter mode, simulated as a
 *        structure-of-arrays pool that splits across the frame's job workers.
 *
 * ## The two modes
 *
 * - **emitter** (Unity's particle system, Blender's Emitter type): particles are born over
 *   time (rate, rate over distance, timed bursts) from the shape, live `start_lifetime`, move
 *   under gravity, forces, drag, turbulence and orbital motion, and die. Colour, size and
 *   rotation evolve over their normalized age.
 * - **scatter** (Blender's Hair type rendered as object instances; Geometry Nodes' "Distribute
 *   Points on Faces" + "Instance on Points"): `count` particles are placed once over the shape
 *   -- typically a mesh's faces, evenly -- and never age or move. Each is oriented to the surface
 *   normal with an optional random spin about it. Over-life curves are sampled at a per-instance
 *   random position instead of age, so a colour gradient or size curve becomes per-instance
 *   *variety* (a meadow of differently tinted, differently sized flowers).
 *
 * ## Determinism and threads
 *
 * Spawning is serial per system and draws from the system's own seeded Rng; the per-particle
 * update reads no RNG at all (anything random after birth comes from a hash of the particle's
 * 32-bit seed), and each worker writes only its own index range. So the simulation is
 * bit-identical whatever the worker count -- `particles_parallel_matches_serial` asserts it.
 * Deaths are compacted serially (swap-remove), after the parallel update.
 *
 * ## Simulation space
 *
 * `world` (the default): particles are born at the emitter's current world pose and then left
 * alone -- a moving torch leaves a trail. New particles are spread along the emitter's path
 * over the frame (sub-frame birth positions and pre-aging), so a fast emitter draws a smooth
 * ribbon rather than per-frame clumps. `local`: positions stay in the emitter's frame and move
 * with it (a scatter always simulates locally).
 */

#ifndef TOYENGINE_PARTICLES_PARTICLE_SYSTEM_H
#define TOYENGINE_PARTICLES_PARTICLE_SYSTEM_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

#include <coopa/scene/component.h>
#include <coopa/scene/components/transform_component.h>
#include <coopa/scene/scene_object.h>

#include <gfxcoopa/engine/components/mesh_renderer.h>

#include <toyengine/particles/particle_math.h>
#include <toyengine/particles/particle_shape.h>
#include <toyengine/render/particle_types.h>

namespace toy {
namespace particles {

/// One range body: handles indices [begin, end).
using RangeFn = std::function<void(std::size_t begin, std::size_t end)>;
/// Runs `fn` over [0, n), each index exactly once, possibly split across threads; returns when done.
using ParallelFor = std::function<void(std::size_t n, const RangeFn& fn)>;

/** @brief `fn` over [0, n): through `par` when given, else inline. */
inline void run_range(const ParallelFor* par, std::size_t n, const RangeFn& fn) {
    if (n == 0) return;
    if (par && *par) (*par)(n, fn);
    else fn(0, n);
}

enum class ParticleMode { Emitter, Scatter };
enum class SimulationSpace { World, Local };
/// Where the particles are simulated: `cpu` (the default) here on the job workers, `gpu` in
/// compute shaders (toyengine/render/passes/gpu_particle_pass.h) -- see ParticleSystem.
enum class SimulationMode { Cpu, Gpu };
enum class SortMode { None, Distance, OldestFirst, YoungestFirst };
enum class FlipbookMode { Lifetime, Random, Fps };

/** @brief A timed burst (Unity's Emission > Bursts): `count` particles at `time` into each
 *         cycle of the system's duration, repeated `cycles` times `interval` apart. */
struct Burst {
    float time = 0.0f;
    Range count{10.0f};
    int   cycles = 1;          ///< 0 = every interval forever (while the system plays).
    float interval = 0.5f;
    float probability = 1.0f;
};

/** @brief On a particle's death, spawn `count` particles in another system at its last
 *         position, inheriting `inherit_velocity` of its velocity (Unity's Sub Emitters). */
struct SubEmitter {
    std::string target;        ///< The other ParticleSystem's object name (found in start()).
    Range count{4.0f};
    float inherit_velocity = 0.0f;
};

/**
 * @struct GroundField
 * @brief A height map particles collide with instead of the flat `ground_height` plane: the
 *        top surface under each cell (roofs, terrain, props), e.g. probed by raycasts straight
 *        down. Set at runtime (ParticleSystem::ground_field), never authored; cells outside it,
 *        or holding NaN (nothing below), use `fallback`.
 */
struct GroundField {
    glm::vec2 origin{0.0f};           ///< World XY of cell (0, 0)'s corner.
    float cell = 1.0f;                ///< Cell edge (m).
    int nx = 0, ny = 0;
    std::vector<float> heights;       ///< nx * ny, row-major in y.
    /// Optional, nx * ny: like `heights` but looking through moving bodies (Rigidbodies) -- what
    /// the renderer's "open to the sky" test for lying snow uses. Empty = same as heights.
    std::vector<float> sky_heights;
    std::vector<glm::vec3> normals;   ///< Optional, nx * ny: the surface's normal (empty = straight up).
    /// Optional, nx * ny: 1 where a landing may fire its sub emitters (splashes), 0 where it
    /// lands silently. Empty: everywhere.
    std::vector<uint8_t> splash;
    float fallback = 0.0f;
    bool fallback_splash = true;      ///< Sub emitters where the fallback plane is the surface.

    /** @brief May a landing at (x, y) splash? */
    bool splashes(float x, float y) const {
        if (splash.empty()) return true;
        const int i = static_cast<int>(std::floor((x - origin.x) / cell));
        const int j = static_cast<int>(std::floor((y - origin.y) / cell));
        if (i < 0 || j < 0 || i >= nx || j >= ny) return fallback_splash;
        return splash[static_cast<size_t>(j) * static_cast<size_t>(nx) + static_cast<size_t>(i)] != 0;
    }

    /** @brief The surface normal under (x, y); up where unknown. */
    glm::vec3 normal(float x, float y) const {
        const int i = static_cast<int>(std::floor((x - origin.x) / cell));
        const int j = static_cast<int>(std::floor((y - origin.y) / cell));
        if (normals.empty() || i < 0 || j < 0 || i >= nx || j >= ny) return glm::vec3(0.0f, 0.0f, 1.0f);
        return normals[static_cast<size_t>(j) * static_cast<size_t>(nx) + static_cast<size_t>(i)];
    }

    float sample(float x, float y) const {
        const int i = static_cast<int>(std::floor((x - origin.x) / cell));
        const int j = static_cast<int>(std::floor((y - origin.y) / cell));
        if (i < 0 || j < 0 || i >= nx || j >= ny) return fallback;
        const float h = heights[static_cast<size_t>(j) * static_cast<size_t>(nx) + static_cast<size_t>(i)];
        return std::isnan(h) ? fallback : h;
    }
};

/**
 * @struct ParticleSettings
 * @brief Everything authored on a ParticleSystem. Grouped like Unity's modules; YAML keys are
 *        the field names (see particle_yaml.h).
 */
struct ParticleSettings {
    // --- Main ---
    ParticleMode    mode = ParticleMode::Emitter;
    float           duration = 5.0f;
    bool            looping = true;
    bool            prewarm = false;      ///< Start as if it had already run one full duration.
    float           start_delay = 0.0f;
    bool            play_on_start = true;
    Range           start_lifetime{2.0f};
    Range           start_speed{1.0f};
    Range           start_size{0.3f};
    Range           start_rotation{0.0f};  ///< Degrees (in-plane quad rotation / spin about the normal).
    glm::vec4       start_color{1.0f};
    glm::vec4       start_color_b{1.0f};  ///< A random mix between start_color and this.
    float           gravity = 0.0f;       ///< Multiplier on 9.81 m/s^2 toward -Z.
    SimulationSpace space = SimulationSpace::World;
    uint32_t        max_particles = 1000;  ///< Pool cap; a GPU system's fixed buffer size.
    /// `gpu`: emit / simulate / sort in compute shaders (high counts: 100k sparks). Falls back to
    /// the CPU, with one warning, for what the GPU path lacks (see gpu_fallback_reason()) or when
    /// `particles.gpu_enabled` is off / the device has no compute.
    SimulationMode  simulation = SimulationMode::Cpu;
    uint32_t        seed = 0;             ///< 0: derived from the object's name.
    float           time_scale = 1.0f;

    // --- Emission ---
    float              rate = 10.0f;                 ///< Particles per second.
    float              rate_over_distance = 0.0f;    ///< Per metre the emitter travels.
    std::vector<Burst> bursts;
    uint32_t           count = 100;                  ///< Scatter mode: instances placed.

    // --- Shape ---
    ShapeSettings shape;
    bool  align_to_normal = false;   ///< Orient each particle to the shape normal (Blender's "Normal").
    bool  random_spin = true;        ///< With align_to_normal: random twist about the normal (Blender's phase).
    float inherit_velocity = 0.0f;   ///< Fraction of the emitter's own velocity given at birth.

    // --- Velocity / forces ---
    glm::vec3 velocity{0.0f};        ///< Constant extra velocity, simulation space (Velocity over Lifetime).
    glm::vec3 force{0.0f};           ///< Constant acceleration, world space (Force over Lifetime, wind).
    float     drag = 0.0f;           ///< Linear damping (1/s).
    float     orbital = 0.0f;        ///< Swirl about the emitter's +Z axis (rad/s).
    float     radial = 0.0f;         ///< Away from (+) / toward (-) the emitter's axis (m/s).
    float     tumble = 0.0f;         ///< 3D tumble about a per-particle random axis (deg/s).
    Range     angular_velocity{0.0f};///< In-plane spin (deg/s).

    // --- Noise (turbulence) ---
    float noise_strength = 0.0f;     ///< m/s^2 of curl-noise swirl.
    float noise_frequency = 0.5f;    ///< Cycles per metre.
    float noise_scroll = 0.5f;       ///< How fast the field evolves.
    int   noise_octaves = 2;

    // --- Over lifetime ---
    Gradient   color_over_life;      ///< Multiplies the start colour; empty: white.
    FloatCurve size_over_life;       ///< Multiplies the start size; empty: 1.
    FloatCurve alpha_over_life;      ///< Extra alpha multiplier; empty: 1. (Simpler than editing gradient alpha.)

    // --- Collision (a world-Z plane) ---
    bool  collide = false;
    float ground_height = 0.0f;
    float bounce = 0.3f;
    float collision_friction = 0.2f; ///< Fraction of horizontal speed lost per bounce.
    bool  kill_on_collide = false;

    // --- Wrap (world space systems: precipitation around a moving camera) ---
    /// Full extents of a box centred on the emitter; > 0 on an axis wraps particles that leave
    /// it back in on the opposite side, so the volume is always full wherever the emitter goes
    /// (rain that keeps up with the camera). 0 = no wrap on that axis.
    glm::vec3 wrap_box{0.0f};
    float wrap_fade = 0.15f;         ///< Fraction of the box's half extents (x, y) over which alpha fades to 0 at its edge.

    // --- Sub emitters ---
    std::vector<SubEmitter> on_death;
    bool on_death_collision_only = false;   ///< Sub emitters fire only for deaths by collision (rain: splashes on landing, none mid-air).

    // --- Renderer ---
    render::ParticleLook look;
    SortMode     sort = SortMode::Distance;
    FlipbookMode flipbook_mode = FlipbookMode::Lifetime;
    float        flipbook_fps = 12.0f;
    float        flipbook_cycles = 1.0f;
    float        max_draw_distance = 0.0f;   ///< Cull beyond this from the camera (m); 0 = never.
};

/** @brief What prepare_render() needs to know about the view. */
struct ParticleView {
    glm::vec3 camera_pos{0.0f};
};

/**
 * @class ParticleSystem
 * @brief The component. Authoring lives in `settings`; playback through play()/stop()/
 *        emit()/clear(); ParticleSimulationSystem (particle_system_runner.h) steps every live
 *        one per frame and Engine collects their draw batches.
 */
class ParticleSystem : public coopa::scene::Component {
public:
    std::string type_name() const override { return "ParticleSystem"; }

    ParticleSettings settings;
    /// Runtime collision surface replacing the ground_height plane (see GroundField); null = the plane.
    std::shared_ptr<const GroundField> ground_field;

    /// Builds (or fetches from the shared cache) the sampling surface of meshes/<key>.yaml --
    /// installed by the YAML parser, which knows the asset roots and the scene's directory. Used
    /// by init() for an emitter mesh named by a sibling MeshRenderer rather than `mesh_path`.
    std::function<std::shared_ptr<const MeshSurface>(const std::string&)> load_surface;

    // ------------------------------------------------------------------ setup

    /** @brief The emission surface (shape `mesh`). Shared: systems emitting from one mesh hold one copy. */
    void set_shape_surface(std::shared_ptr<const MeshSurface> surface) {
        surface_ = std::move(surface);
        const bool ready = surface_ && !surface_->empty();
        if (ready && !surface_ready_ && settings.mode == ParticleMode::Scatter) scattered_ = false;   // re-place
        surface_ready_ = ready;
    }
    /** @brief Hand over a surface built by hand (tests, procedural emitters). */
    void set_shape_surface(MeshSurface surface) { set_shape_surface(std::make_shared<const MeshSurface>(std::move(surface))); }
    const MeshSurface* shape_surface() const { return surface_.get(); }
    bool shape_surface_ready() const { return surface_ready_; }

    /** @brief Mesh render mode's mesh + material, as a MeshRenderer that is never attached. */
    coopa::gfx::engine::components::MeshRenderer& mesh_proxy() {
        if (!proxy_) proxy_ = std::make_unique<coopa::gfx::engine::components::MeshRenderer>();
        return *proxy_;
    }
    bool has_mesh_proxy() const { return proxy_ != nullptr; }

    /** @brief The optional texture (albedo slot) for `sprite: texture`. */
    coopa::gfx::engine::components::PBRMaterial texture_material;
    bool has_texture() const { return texture_material.has_albedo_map(); }

    /** @brief Re-bakes settings-derived state (noise field); call after editing `settings` by hand. */
    void apply_settings() {
        settings.color_over_life.bake();
        settings.size_over_life.bake();
        settings.alpha_over_life.bake();
        turbulence_.configure(effective_seed_(), std::max(settings.noise_frequency, 1e-3f), settings.noise_octaves);
        settings_applied_ = true;
    }

    void start() override { init(); }

    /**
     * @brief One-time setup: seed, emitter mesh, mesh proxy owner, play_on_start. Idempotent --
     *        Component::start() calls it in play mode, and ParticleSimulationSystem calls it
     *        for systems in a scene that is only being edited (which never start()s).
     */
    void init() {
        if (started_) return;
        started_ = true;
        rng_.reseed(effective_seed_());
        // A shape `mesh` with no mesh_path borrows the sibling MeshRenderer's mesh -- Blender's
        // emitter IS the object's mesh, so this is the natural default.
        if (settings.shape.type == EmitShape::Mesh && !surface_ready_ && owner && load_surface) {
            std::string key = settings.shape.mesh_path;
            if (key.empty()) {
                if (auto* mr = owner->get_component<coopa::gfx::engine::components::MeshRenderer>()) key = mr->mesh_path();
            }
            if (!key.empty()) set_shape_surface(load_surface(key));
        }
        if (proxy_) proxy_->owner = owner;
        apply_settings();
        if (settings.play_on_start) play();
    }
    bool initialized() const { return started_; }

    // ------------------------------------------------------------------ playback

    void play() {
        if (!settings_applied_) apply_settings();
        playing_ = true;
        emitting_ = true;
        if (time_ <= 0.0f && settings.prewarm && settings.mode == ParticleMode::Emitter) prewarm_pending_ = true;
    }
    /** @brief Stops emitting; live particles finish their lives unless `clear_particles`. */
    void stop(bool clear_particles = false) {
        emitting_ = false;
        if (clear_particles) {
            clear();
            playing_ = false;   // a scatter would otherwise re-place itself next step
        }
    }
    void pause(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }
    void clear() {
        // A GPU system resets its buffers on the next job (new generation).
        ++gpu_generation_;
        gpu_alive_ = 0;
        gpu_spawn_ = 0;
        gpu_dt_ = 0.0f;
        pool_.clear();
        pending_at_.clear();
        pending_emit_ = 0;
        scattered_ = false;
    }
    /** @brief Restart from t = 0 (clears particles and re-arms bursts). */
    void restart() {
        clear();
        time_ = 0.0f;
        emit_accum_ = 0.0f;
        has_prev_world_ = false;
        rng_.reseed(effective_seed_());
        play();
    }
    /** @brief Spawns `n` particles from the shape right now (next step), regardless of rate. */
    void emit(uint32_t n) { pending_emit_ += n; }
    /** @brief Spawns one particle at a WORLD position with a world velocity (sub emitters). */
    /**
     * @brief Spawns one particle at a world position next step (what sub emitters use). `normal`
     *        is the surface it came from: with align_to_normal the particle lies in that surface
     *        (a splash ring flat on a sloped roof) and its start speed points off it.
     */
    void emit_at(const glm::vec3& world_pos, const glm::vec3& world_vel, const glm::vec3& normal = glm::vec3(0.0f, 0.0f, 1.0f)) {
        pending_at_.push_back({world_pos, world_vel, normal});
    }

    bool is_playing() const { return playing_; }
    bool is_emitting() const { return playing_ && emitting_; }
    /** @brief Still has something to do: emitting, or particles alive. */
    bool is_alive() const {
        return is_emitting() || !pool_.empty() || !pending_at_.empty() || pending_emit_ > 0 ||
               (gpu_active_ && (gpu_alive_ > 0 || gpu_spawn_ > 0));
    }
    /** @brief Live particles: the pool's size, or for a GPU system the last read-back alive
     *         count (a couple of frames old). */
    uint32_t particle_count() const { return gpu_active_ ? gpu_alive_ : static_cast<uint32_t>(pool_.size()); }
    float time() const { return time_; }

    // ------------------------------------------------------------------ simulation

    /**
     * @brief Advances the system by `dt` seconds with the emitter at `world` this frame.
     *        `sim_time` is the global clock the turbulence field scrolls on. Serial spawn,
     *        then the update split over `par`, then a serial compaction.
     */
    void step(float dt, const glm::mat4& world, const ParallelFor* par = nullptr) {
        if (!settings_applied_) apply_settings();
        world_ = world;
        if (!has_prev_world_) { prev_world_ = world; has_prev_world_ = true; }
        dead_events_.clear();

        if (prewarm_pending_) {
            prewarm_pending_ = false;
            const float step_dt = 1.0f / 30.0f;
            const int steps = static_cast<int>(std::ceil(std::max(settings.duration, 0.0f) / step_dt));
            for (int i = 0; i < steps; ++i) step_once_(step_dt, par, true);
        }
        if (gpu_active_) {
            step_gpu_(paused_ ? 0.0f : dt * std::max(settings.time_scale, 0.0f));
            prev_world_ = world_;
            return;
        }
        if (paused_ || dt <= 0.0f) {
            prev_world_ = world_;
            return;
        }
        step_once_(dt * std::max(settings.time_scale, 0.0f), par, true);
        prev_world_ = world_;
    }

    /** @brief World-space AABB of the live particles after the last step (sizes included). */
    glm::vec3 bounds_min() const { return bounds_min_; }
    glm::vec3 bounds_max() const { return bounds_max_; }

    /** @brief Deaths this step that feed a sub emitter: (index into settings.on_death, pos, vel). */
    /// `normal`: of the surface it landed on (a collision death), else straight up.
    struct DeathEvent { uint32_t sub = 0; glm::vec3 pos{0.0f}; glm::vec3 vel{0.0f}; glm::vec3 normal{0.0f, 0.0f, 1.0f}; };
    const std::vector<DeathEvent>& death_events() const { return dead_events_; }

    /// Sub emitter targets, resolved by ParticleSimulationSystem (parallel to settings.on_death).
    std::vector<ParticleSystem*> sub_targets;

    // ------------------------------------------------------------------ rendering

    /**
     * @brief Fills this frame's GPU instances (quads) or instance matrices (mesh mode), sorted
     *        as `settings.sort` asks. Reads only this system; safe to run for several systems
     *        at once, and itself splits over `par` when large.
     */
    void prepare_render(const ParticleView& view, const ParallelFor* par = nullptr) {
        const size_t n = pool_.size();
        const bool mesh_mode = settings.look.mode == render::ParticleRenderMode::Mesh;
        instances_.resize(mesh_mode ? 0 : n);
        matrices_.resize(mesh_mode ? n : 0);
        if (n == 0 || settings.look.mode == render::ParticleRenderMode::None) {
            instances_.clear();
            matrices_.clear();
            return;
        }

        const bool local = simulates_locally_();
        const glm::mat4 to_world = local ? world_ : glm::mat4(1.0f);
        const glm::mat3 rot_world = local ? glm::mat3(world_) : glm::mat3(1.0f);
        const glm::quat emitter_rot = local ? glm::quat_cast(orthonormal_(rot_world)) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
        const float scale_world = local ? std::cbrt(std::abs(glm::determinant(rot_world))) : 1.0f;
        const glm::vec2 half_wrap = local ? glm::vec2(0.0f) : glm::vec2(settings.wrap_box) * 0.5f;
        const float fade_frac = std::clamp(settings.wrap_fade, 1e-3f, 1.0f);
        const bool wrap_fade = settings.wrap_fade > 0.0f && (half_wrap.x > 0.0f || half_wrap.y > 0.0f);

        // Draw order. Sorting needs the world position, so compute keys first.
        order_.resize(n);
        std::iota(order_.begin(), order_.end(), 0u);
        if (!mesh_mode && settings.sort != SortMode::None) {
            keys_.resize(n);
            const glm::vec3 eye = view.camera_pos;
            run_range(par, n, [&](size_t b, size_t e) {
                for (size_t i = b; i < e; ++i) {
                    switch (settings.sort) {
                        case SortMode::Distance: {
                            const glm::vec3 p = glm::vec3(to_world * glm::vec4(pool_.pos[i], 1.0f)) - eye;
                            keys_[i] = -glm::dot(p, p);      // farthest first
                            break;
                        }
                        case SortMode::OldestFirst:   keys_[i] = -pool_.age[i]; break;
                        case SortMode::YoungestFirst: keys_[i] = pool_.age[i]; break;
                        default: keys_[i] = 0.0f; break;
                    }
                }
            });
            radix_sort_order_();
        }

        const bool scatter = settings.mode == ParticleMode::Scatter;
        const float frames = std::max(1.0f, settings.look.flipbook.x * settings.look.flipbook.y);
        const float tumble = glm::radians(settings.tumble);
        run_range(par, n, [&](size_t b, size_t e) {
            for (size_t k = b; k < e; ++k) {
                const uint32_t i = order_[k];
                const float t = life_t_(i, scatter);
                const float size = pool_.size0[i] * settings.size_over_life.evaluate(t) * scale_world;
                glm::vec4 color = pool_.color0[i] * settings.color_over_life.evaluate(t);
                color.a *= settings.alpha_over_life.evaluate(t);
                const glm::vec3 wp = glm::vec3(to_world * glm::vec4(pool_.pos[i], 1.0f));
                if (wrap_fade) {   // fade toward the wrap box's sides, so a wrap is never seen
                    const glm::vec2 d = glm::abs(glm::vec2(wp) - glm::vec2(world_[3]));
                    float f = 1.0f;
                    for (int a = 0; a < 2; ++a) {
                        if (half_wrap[a] <= 0.0f) continue;
                        const float edge = (half_wrap[a] - d[a]) / (half_wrap[a] * fade_frac);
                        f *= std::clamp(edge, 0.0f, 1.0f);
                    }
                    color.a *= f;
                }
                glm::quat q = emitter_rot * pool_.orient[i];
                if (tumble != 0.0f) {
                    const uint32_t s = pool_.seed[i];
                    const glm::vec3 axis = glm::normalize(glm::vec3(hash01(s * 3u + 11u) - 0.5f, hash01(s * 3u + 12u) - 0.5f,
                                                                    hash01(s * 3u + 13u) - 0.5f) + glm::vec3(0.0f, 0.0f, 1e-4f));
                    q = q * glm::angleAxis(tumble * pool_.age[i], axis);
                }
                if (mesh_mode) {
                    const glm::quat spin = glm::angleAxis(pool_.rot[i], glm::vec3(0.0f, 0.0f, 1.0f));
                    glm::mat4 m = glm::mat4_cast(q * spin);
                    m[0] *= size; m[1] *= size; m[2] *= size;
                    m[3] = glm::vec4(wp, 1.0f);
                    matrices_[k] = m;
                    continue;
                }
                render::ParticleInstance& inst = instances_[k];
                inst.pos_size = glm::vec4(wp, size);
                inst.color = color;
                // The visible motion (step()'s dp) includes the constant `velocity`, so stretching does too.
                inst.velocity_rot = glm::vec4(rot_world * (pool_.vel[i] + settings.velocity), pool_.rot[i]);
                inst.orient = glm::vec4(q.x, q.y, q.z, q.w);
                const float rnd = hash01(pool_.seed[i]);
                float frame = 0.0f;
                if (frames > 1.0f) {
                    switch (settings.flipbook_mode) {
                        case FlipbookMode::Random:   frame = std::floor(rnd * frames); break;
                        case FlipbookMode::Fps:      frame = std::fmod(std::floor(pool_.age[i] * settings.flipbook_fps + rnd * frames), frames); break;
                        case FlipbookMode::Lifetime:
                        default:                     frame = std::fmod(std::floor(t * frames * std::max(settings.flipbook_cycles, 1e-3f)), frames); break;
                    }
                }
                inst.misc = glm::vec4(t, rnd, frame, 0.0f);
            }
        });
    }

    const std::vector<render::ParticleInstance>& instances() const { return instances_; }
    const std::vector<glm::mat4>& instance_matrices() const { return matrices_; }

    /** @brief Bounds centre -- where the batch sorts among other transparent geometry. */
    glm::vec3 sort_center() const { return 0.5f * (bounds_min_ + bounds_max_); }

    /** @brief Emitter world position (for culling by distance). */
    glm::vec3 emitter_position() const { return glm::vec3(world_[3]); }

    // ------------------------------------------------------------------ GPU simulation

    /**
     * @brief Why this system cannot simulate on the GPU, or empty when it can. The GPU path
     *        covers emitters with every analytic shape and mesh faces, forces, drag, noise,
     *        orbital / radial, the over-life curves, ground-plane collision, wrap and sorting;
     *        not sub emitters (either end), scatter, mesh render mode, prewarm, a runtime
     *        GroundField or mesh vertex / edge emission.
     */
    std::string gpu_fallback_reason(bool is_sub_target) const {
        if (settings.mode == ParticleMode::Scatter) return "scatter mode";
        if (settings.look.mode == render::ParticleRenderMode::Mesh) return "mesh render mode";
        if (!settings.on_death.empty()) return "sub emitters (on_death)";
        if (is_sub_target) return "it is another system's sub emitter";
        if (settings.prewarm) return "prewarm";
        if (ground_field) return "a runtime ground field";
        if (settings.shape.type == EmitShape::Mesh && settings.shape.emit_from != MeshEmitFrom::Faces)
            return "mesh emit_from vertices / edges";
        return {};
    }
    /** @brief Set by ParticleSimulationSystem each step; switching either way clears the pool. */
    void set_gpu_active(bool on) {
        if (on == gpu_active_) return;
        clear();
        gpu_active_ = on;
    }
    bool gpu_active() const { return gpu_active_; }
    /// The runner's once-per-system fallback warning has been printed.
    bool gpu_fallback_warned = false;
    /** @brief The renderer's read-back alive count; ignored unless it is of this generation. */
    void set_gpu_alive(uint32_t count, uint32_t generation) {
        if (gpu_active_ && generation == gpu_generation_) gpu_alive_ = count;
    }
    uint64_t gpu_id() const { return gpu_id_; }

    /**
     * @brief This frame's GPU job: the births and time accumulated since the last job, and every
     *        setting as GpuParticleParams. False when idle (stopped with nothing alive).
     */
    bool make_gpu_job(const ParticleView& view, render::GpuParticleJob& job) {
        if (!gpu_active_ || !started_) return false;
        if (!playing_ && gpu_alive_ == 0 && gpu_spawn_ == 0) {
            gpu_dt_ = 0.0f;
            return false;
        }
        static std::atomic<uint64_t> next_id{1};
        if (gpu_id_ == 0) gpu_id_ = next_id.fetch_add(1);
        const ParticleSettings& s = settings;
        const bool local = simulates_locally_();
        if (!has_job_world_) { job_prev_world_ = world_; has_job_world_ = true; }
        job.id = gpu_id_;
        job.generation = gpu_generation_;
        job.capacity = std::max(1u, s.max_particles);
        job.sort = s.sort != SortMode::None && s.look.additive < 0.999f;
        job.triangles.reset();
        render::GpuParticleParams& P = job.params;
        const float dt = gpu_dt_;
        P.world = world_;
        P.prev_world = job_prev_world_;
        P.to_world = local ? world_ : glm::mat4(1.0f);
        // A float in [1, 2): 23 random mantissa bits the shader hashes, never a NaN pattern.
        const uint32_t seed_bits = (rng_.next_u32() & 0x007FFFFFu) | 0x3F800000u;
        float seed_f;
        std::memcpy(&seed_f, &seed_bits, sizeof(seed_f));
        P.frame = glm::vec4(dt, sim_clock_ * s.noise_scroll, static_cast<float>(std::min(gpu_spawn_, job.capacity)), seed_f);
        const float sort_mode = !job.sort ? 0.0f : s.sort == SortMode::Distance ? 1.0f : s.sort == SortMode::OldestFirst ? 2.0f : 3.0f;
        const float scale_world = local ? std::cbrt(std::abs(glm::determinant(glm::mat3(world_)))) : 1.0f;
        P.config = glm::vec4(static_cast<float>(job.capacity), sort_mode, local ? 1.0f : 0.0f, scale_world);
        P.eye = glm::vec4(view.camera_pos, std::max(1.0f, s.look.flipbook.x * s.look.flipbook.y));
        const ShapeSettings& sh = s.shape;
        P.shape0 = glm::vec4(static_cast<float>(sh.type), sh.radius, sh.radius_thickness, sh.angle_deg);
        P.shape1 = glm::vec4(sh.arc_deg, sh.random_direction, sh.normal_offset, sh.length);
        P.shape_box = glm::vec4(sh.box, 0.0f);
        P.shape_offset = glm::vec4(sh.offset, 0.0f);
        if (sh.type == EmitShape::Mesh && surface_ready_) {
            if (gpu_tris_src_ != surface_.get()) {
                gpu_tris_ = std::make_shared<const std::vector<glm::vec4>>(surface_->export_faces());
                gpu_tris_src_ = surface_.get();
            }
            job.triangles = gpu_tris_;
            P.shape_box.w = static_cast<float>(surface_->triangle_count());
            P.shape_offset.w = surface_->area();
        }
        P.life_speed = glm::vec4(s.start_lifetime.min, s.start_lifetime.max, s.start_speed.min, s.start_speed.max);
        P.size_rot = glm::vec4(s.start_size.min, s.start_size.max, glm::radians(s.start_rotation.min), glm::radians(s.start_rotation.max));
        P.spin = glm::vec4(glm::radians(s.angular_velocity.min), glm::radians(s.angular_velocity.max),
                           s.align_to_normal ? 1.0f : 0.0f, s.random_spin ? 1.0f : 0.0f);
        P.color_a = s.start_color;
        P.color_b = s.start_color_b;
        const glm::mat3 inv_rot = local ? glm::inverse(glm::mat3(world_)) : glm::mat3(1.0f);
        P.accel = glm::vec4(inv_rot * (glm::vec3(0.0f, 0.0f, -9.81f * s.gravity) + s.force), s.drag);
        P.velocity = glm::vec4(s.velocity, s.orbital);
        P.center = glm::vec4(local ? glm::vec3(0.0f) : glm::vec3(world_[3]), s.radial);
        P.axis = glm::vec4(local ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::normalize(glm::mat3(world_)[2] + glm::vec3(0.0f, 0.0f, 1e-8f)),
                           glm::radians(s.tumble));
        P.collision = glm::vec4(s.collide ? 1.0f : 0.0f, s.ground_height, s.bounce, 1.0f - std::clamp(s.collision_friction, 0.0f, 1.0f));
        P.wrap = glm::vec4(local ? glm::vec3(0.0f) : glm::max(s.wrap_box, glm::vec3(0.0f)), std::max(s.wrap_fade, 0.0f));
        P.noise = glm::vec4(s.noise_strength, s.kill_on_collide ? 1.0f : 0.0f, static_cast<float>(s.flipbook_mode), s.flipbook_fps);
        glm::vec3 inherit(0.0f);
        if (!local && s.inherit_velocity != 0.0f && dt > 0.0f) {
            inherit = (glm::vec3(world_[3]) - glm::vec3(job_prev_world_[3])) / dt * s.inherit_velocity;
        }
        P.misc = glm::vec4(s.flipbook_cycles, inherit);
        const glm::quat er = glm::quat_cast(orthonormal_(glm::mat3(world_)));
        P.emitter_rot = glm::vec4(er.x, er.y, er.z, er.w);
        turbulence_.export_waves(P.noise_k, P.noise_c);
        for (int i = 0; i < 64; ++i) {
            const float t = static_cast<float>(i) / 63.0f;
            P.color_lut[i] = s.color_over_life.evaluate(t);
            P.curve_lut[i] = glm::vec4(s.size_over_life.evaluate(t), s.alpha_over_life.evaluate(t), 0.0f, 0.0f);
        }
        gpu_spawn_ = 0;
        gpu_dt_ = 0.0f;
        job_prev_world_ = world_;
        return true;
    }

    /// Raw pool access for tests and tools.
    struct Pool {
        std::vector<glm::vec3> pos, vel;
        std::vector<float>     age, life, size0, rot, rot_vel;
        std::vector<glm::vec4> color0;
        std::vector<glm::quat> orient;
        std::vector<uint32_t>  seed;

        size_t size() const { return pos.size(); }
        bool empty() const { return pos.empty(); }
        void clear() {
            pos.clear(); vel.clear(); age.clear(); life.clear(); size0.clear(); rot.clear(); rot_vel.clear();
            color0.clear(); orient.clear(); seed.clear();
        }
        void reserve(size_t n) {
            pos.reserve(n); vel.reserve(n); age.reserve(n); life.reserve(n); size0.reserve(n); rot.reserve(n);
            rot_vel.reserve(n); color0.reserve(n); orient.reserve(n); seed.reserve(n);
        }
        void swap_remove(size_t i) {
            const size_t last = pos.size() - 1;
            if (i != last) {
                pos[i] = pos[last]; vel[i] = vel[last]; age[i] = age[last]; life[i] = life[last];
                size0[i] = size0[last]; rot[i] = rot[last]; rot_vel[i] = rot_vel[last];
                color0[i] = color0[last]; orient[i] = orient[last]; seed[i] = seed[last];
            }
            pos.pop_back(); vel.pop_back(); age.pop_back(); life.pop_back(); size0.pop_back(); rot.pop_back();
            rot_vel.pop_back(); color0.pop_back(); orient.pop_back(); seed.pop_back();
        }
    };
    const Pool& pool() const { return pool_; }

private:
    struct PendingAt { glm::vec3 pos; glm::vec3 vel; glm::vec3 normal; };

    uint32_t effective_seed_() const {
        if (settings.seed != 0) return settings.seed;
        // FNV-1a of the object's name: stable across runs and builds, distinct per emitter.
        uint32_t h = 2166136261u;
        if (owner) for (char c : owner->name()) { h ^= static_cast<uint8_t>(c); h *= 16777619u; }
        return h ? h : 1u;
    }

    bool simulates_locally_() const {
        return settings.space == SimulationSpace::Local || settings.mode == ParticleMode::Scatter;
    }

    /**
     * @brief Sorts order_ (initially 0..n-1) ascending by keys_, ties in index order: a stable
     *        3-pass LSD radix sort (11 bits a pass) over the keys mapped to order-preserving
     *        unsigned ints. Linear time -- at 20k particles it is several times faster than a
     *        comparison sort through the key indirection, and it is serial and exact, so the
     *        draw order stays identical on any worker count.
     */
    void radix_sort_order_() {
        const size_t n = order_.size();
        sort_keys_.resize(n);
        sort_tmp_keys_.resize(n);
        sort_tmp_order_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            uint32_t u;
            std::memcpy(&u, &keys_[order_[i]], sizeof(u));
            // IEEE-754 to an unsigned int with the same ordering: flip every bit of a negative,
            // only the sign bit of a positive.
            sort_keys_[i] = (u & 0x80000000u) ? ~u : (u | 0x80000000u);
        }
        for (int pass = 0; pass < 3; ++pass) {
            const int shift = pass * 11;
            uint32_t count[2048] = {};
            for (size_t i = 0; i < n; ++i) ++count[(sort_keys_[i] >> shift) & 2047u];
            uint32_t sum = 0;
            for (uint32_t& c : count) { const uint32_t t = c; c = sum; sum += t; }
            for (size_t i = 0; i < n; ++i) {
                const uint32_t dst = count[(sort_keys_[i] >> shift) & 2047u]++;
                sort_tmp_keys_[dst] = sort_keys_[i];
                sort_tmp_order_[dst] = order_[i];
            }
            sort_keys_.swap(sort_tmp_keys_);
            order_.swap(sort_tmp_order_);
        }
    }

    float life_t_(uint32_t i, bool scatter) const {
        if (scatter) return hash01(pool_.seed[i] ^ 0x5bd1e995u);
        const float l = pool_.life[i];
        return l > 0.0f ? std::clamp(pool_.age[i] / l, 0.0f, 1.0f) : 1.0f;
    }

    static glm::mat3 orthonormal_(const glm::mat3& m) {
        glm::vec3 x = m[0], y = m[1];
        const float lx = glm::length(x);
        x = lx > 1e-8f ? x / lx : glm::vec3(1.0f, 0.0f, 0.0f);
        y = y - x * glm::dot(x, y);
        const float ly = glm::length(y);
        y = ly > 1e-8f ? y / ly : glm::vec3(0.0f, 1.0f, 0.0f);
        glm::vec3 z = glm::cross(x, y);
        if (glm::dot(z, m[2]) < 0.0f) z = -z;
        return glm::mat3(x, y, z);
    }

    /** @brief One shape sample in emitter-local space. `u` drives the mesh CDF (stratified for even). */
    EmitSample sample_shape_(float u) {
        const ShapeSettings& s = settings.shape;
        EmitSample e;
        if (s.type == EmitShape::Mesh) {
            if (!surface_ready_) return e;   // no surface yet: nothing to spawn from (callers check)
            e = surface_->sample(s.emit_from, u, rng_);
        } else {
            e = sample_analytic_shape(s, rng_);
        }
        finish_shape_sample(s, e, rng_);
        return e;
    }

    bool can_spawn_() const {
        return settings.shape.type != EmitShape::Mesh || surface_ready_;
    }

    /**
     * @brief Appends one particle. `frac` in [0, 1] is how far back in this frame it was born (1 =
     *        at the frame's start), used to place it along the emitter's path and pre-age it.
     */
    void spawn_(float dt, float frac, const EmitSample* forced = nullptr, const glm::vec3* forced_vel = nullptr,
                float u = -1.0f) {
        if (pool_.size() >= settings.max_particles) return;
        const bool local = simulates_locally_();
        EmitSample e;
        if (forced) e = *forced;
        else e = sample_shape_(u >= 0.0f ? u : rng_.next01());

        const float speed = settings.start_speed.sample(rng_);
        glm::vec3 pos = e.position;
        glm::vec3 vel = e.direction * speed;
        glm::quat q(1.0f, 0.0f, 0.0f, 0.0f);
        if (settings.align_to_normal) {
            q = quat_from_normal_tangent(e.normal, e.tangent);
            if (settings.random_spin) q = q * glm::angleAxis(rng_.next01() * 6.28318530718f, glm::vec3(0.0f, 0.0f, 1.0f));
        }
        if (!local && !forced) {
            // Into world space, along the emitter's path over this frame.
            const glm::vec3 p_now = glm::vec3(world_ * glm::vec4(pos, 1.0f));
            const glm::vec3 p_then = glm::vec3(prev_world_ * glm::vec4(pos, 1.0f));
            pos = glm::mix(p_now, p_then, frac);
            vel = glm::mat3(world_) * vel;
            const float len = glm::length(vel);
            const float want = speed;
            if (len > 1e-6f) vel *= want / len;   // world scale must not change speed
            q = glm::quat_cast(orthonormal_(glm::mat3(world_))) * q;
            if (settings.inherit_velocity != 0.0f && dt > 0.0f) {
                vel += (glm::vec3(world_[3]) - glm::vec3(prev_world_[3])) / dt * settings.inherit_velocity;
            }
        }
        if (forced_vel) vel = *forced_vel;
        // Born under cover (inside a house, below terrain): never spawned at all.
        if (!local && settings.collide && ground_field && pos.z < ground_field->sample(pos.x, pos.y)) return;

        const uint32_t seed = rng_.next_u32();
        const float life = std::max(settings.start_lifetime.sample(rng_), 1e-3f);
        const float c = rng_.next01();
        pool_.pos.push_back(pos);
        pool_.vel.push_back(vel);
        pool_.age.push_back(0.0f);
        pool_.life.push_back(settings.mode == ParticleMode::Scatter ? std::numeric_limits<float>::infinity() : life);
        pool_.size0.push_back(std::max(0.0f, settings.start_size.sample(rng_)));
        pool_.rot.push_back(glm::radians(settings.start_rotation.sample(rng_)));
        pool_.rot_vel.push_back(glm::radians(settings.angular_velocity.sample(rng_)));
        pool_.color0.push_back(glm::mix(settings.start_color, settings.start_color_b, c));
        pool_.orient.push_back(q);
        pool_.seed.push_back(seed);

        // Pre-age a particle born partway through the frame so a stream stays continuous.
        if (frac > 0.0f && dt > 0.0f && settings.mode == ParticleMode::Emitter) {
            const size_t i = pool_.size() - 1;
            const float pre = frac * dt;
            pool_.pos[i] += pool_.vel[i] * pre;
            pool_.age[i] = pre;
        }
    }

    /** @brief How many particles the rate, distance and bursts ask for over [t0, t1). */
    uint32_t emission_count_(float t0, float t1) {
        if (!is_emitting()) return 0;
        const float delay = settings.start_delay;
        if (t1 <= delay) return 0;
        // System-local time, after the delay.
        const float a = std::max(t0 - delay, 0.0f);
        const float b = t1 - delay;
        const float dur = std::max(settings.duration, 1e-3f);
        if (!settings.looping && a >= dur) {
            emitting_ = false;
            return 0;
        }
        const float b_emit = settings.looping ? b : std::min(b, dur);
        float emitted = std::max(0.0f, b_emit - a) * settings.rate;
        if (settings.rate_over_distance > 0.0f) {
            emitted += glm::distance(glm::vec3(world_[3]), glm::vec3(prev_world_[3])) * settings.rate_over_distance;
        }
        emit_accum_ += emitted;
        uint32_t n = static_cast<uint32_t>(emit_accum_);
        emit_accum_ -= static_cast<float>(n);

        // Bursts, stateless: every firing time that falls in [a, b). A finite burst repeats its
        // `cycles` firings each loop of the duration; `cycles: 0` fires every interval forever.
        auto fire = [&](const Burst& br) {
            if (rng_.next01() <= br.probability) {
                n += static_cast<uint32_t>(std::max(0.0f, std::round(br.count.sample(rng_))));
            }
        };
        const float b_burst = settings.looping ? b : std::min(b, dur);
        for (const Burst& br : settings.bursts) {
            const float interval = std::max(br.interval, 1e-3f);
            if (br.cycles <= 0) {
                const float first = std::max(0.0f, std::ceil((a - br.time) / interval));
                for (float k = first;; k += 1.0f) {
                    const float t = br.time + k * interval;
                    if (t >= b_burst) break;
                    if (t >= a) fire(br);
                }
                continue;
            }
            const int loop0 = settings.looping ? static_cast<int>(std::floor(a / dur)) : 0;
            const int loop1 = settings.looping ? static_cast<int>(std::floor(b_burst / dur)) : 0;
            for (int loop = loop0; loop <= loop1; ++loop) {
                for (int k = 0; k < br.cycles; ++k) {
                    const float within = br.time + static_cast<float>(k) * interval;
                    if (within >= dur && settings.looping) break;
                    const float t = static_cast<float>(loop) * dur + within;
                    if (t >= a && t < b_burst) fire(br);
                }
            }
        }
        return n;
    }

    void step_once_(float dt, const ParallelFor* par, bool emit) {
        const float t0 = time_;
        time_ += dt;
        sim_clock_ += dt;

        // 1. Update every live particle (parallel: each index writes only itself).
        update_(dt, par);

        // 2. Deaths, compacted serially. Feed sub emitters first.
        for (size_t i = pool_.size(); i-- > 0;) {
            if (pool_.age[i] < pool_.life[i]) continue;
            // A silent death (born or wrapped under cover, see update_()) feeds no sub emitter.
            const bool silent = pool_.age[i] == k_silent_death_ ||
                                (settings.on_death_collision_only && pool_.age[i] != k_collide_death_) ||
                                // Landed on a surface that takes no splashes (GroundField::splash).
                                (pool_.age[i] == k_collide_death_ && ground_field &&
                                 !ground_field->splashes(pool_.pos[i].x, pool_.pos[i].y));
            const uint32_t subs = silent ? 0u : static_cast<uint32_t>(settings.on_death.size());
            // A landing passes on the surface it hit (the height map's normal, else the flat ground).
            const glm::vec3 n = (subs > 0 && pool_.age[i] == k_collide_death_ && ground_field)
                ? ground_field->normal(pool_.pos[i].x, pool_.pos[i].y) : glm::vec3(0.0f, 0.0f, 1.0f);
            for (uint32_t s = 0; s < subs; ++s) {
                const bool local = simulates_locally_();
                const glm::vec3 wp = local ? glm::vec3(world_ * glm::vec4(pool_.pos[i], 1.0f)) : pool_.pos[i];
                const glm::vec3 wv = local ? glm::mat3(world_) * pool_.vel[i] : pool_.vel[i];
                dead_events_.push_back({s, wp, wv, n});
            }
            pool_.swap_remove(i);
        }

        // 3. Births.
        if (settings.mode == ParticleMode::Scatter) {
            if (!scattered_ && can_spawn_() && playing_) {
                pool_.clear();
                rng_.reseed(effective_seed_());
                const uint32_t n = std::min(settings.count, settings.max_particles);
                pool_.reserve(n);
                const bool even = settings.shape.distribution == MeshDistribution::Even;
                for (uint32_t i = 0; i < n; ++i) {
                    const float u = even ? (static_cast<float>(i) + rng_.next01()) / static_cast<float>(n) : rng_.next01();
                    spawn_(0.0f, 0.0f, nullptr, nullptr, u);
                }
                scattered_ = true;
            }
        } else if (emit) {
            for (const PendingAt& p : pending_at_) {
                EmitSample e;
                e.position = simulates_locally_() ? glm::vec3(glm::inverse(world_) * glm::vec4(p.pos, 1.0f)) : p.pos;
                const glm::vec3 v = simulates_locally_() ? glm::inverse(glm::mat3(world_)) * p.vel : p.vel;
                // The surface it came from: the particle's orientation (align_to_normal) and, then,
                // the hemisphere its start speed points into.
                e.normal = glm::normalize(simulates_locally_() ? glm::inverse(glm::mat3(world_)) * p.normal : p.normal);
                e.tangent = glm::normalize(std::abs(e.normal.z) < 0.9f ? glm::cross(e.normal, glm::vec3(0.0f, 0.0f, 1.0f))
                                                                        : glm::cross(e.normal, glm::vec3(1.0f, 0.0f, 0.0f)));
                // A sub-emitter spawn: forced position, its own start speed added along a random
                // direction -- off the surface when aligned to it.
                const float sp = settings.start_speed.sample(rng_);
                glm::vec3 dir = rng_.unit_vector();
                if (settings.align_to_normal) {
                    if (glm::dot(dir, e.normal) < 0.0f) dir = -dir;
                    dir = glm::normalize(dir + e.normal * 0.6f);
                }
                const glm::vec3 vel = v + dir * sp;
                spawn_(dt, 0.0f, &e, &vel);
            }
            pending_at_.clear();
            if (can_spawn_()) {
                uint32_t n = emission_count_(t0, time_) + pending_emit_;
                pending_emit_ = 0;
                n = std::min<uint32_t>(n, settings.max_particles > pool_.size() ? settings.max_particles - static_cast<uint32_t>(pool_.size()) : 0u);
                const bool even = settings.shape.distribution == MeshDistribution::Even;
                for (uint32_t i = 0; i < n; ++i) {
                    const float frac = n > 1 ? 1.0f - (static_cast<float>(i) + 0.5f) / static_cast<float>(n) : 0.0f;
                    const float u = even ? (static_cast<float>(i) + rng_.next01()) / static_cast<float>(n) : rng_.next01();
                    spawn_(dt, frac, nullptr, nullptr, u);
                }
            }
            if (!settings.looping && time_ - settings.start_delay >= settings.duration) emitting_ = false;
        }

        compute_bounds_(par);
    }

    /** @brief A GPU system's CPU half: the clock, the emission count (rate, distance, bursts --
     *         the same emission_count_() as the CPU path) and conservative bounds. */
    void step_gpu_(float dt) {
        if (dt > 0.0f) {
            const float t0 = time_;
            time_ += dt;
            sim_clock_ += dt;
            pending_at_.clear();
            if (can_spawn_()) {
                const uint64_t n = static_cast<uint64_t>(emission_count_(t0, time_)) + pending_emit_;
                pending_emit_ = 0;
                gpu_spawn_ = static_cast<uint32_t>(std::min<uint64_t>(gpu_spawn_ + n, std::max(1u, settings.max_particles)));
            }
            if (!settings.looping && time_ - settings.start_delay >= settings.duration) emitting_ = false;
            gpu_dt_ += dt;
        }
        compute_gpu_bounds_();
    }

    /**
     * @brief Bounds a GPU system's particles cannot leave, from the settings alone (nothing is
     *        read back): per axis, the extremes of v0 t + a t^2 / 2 over a lifetime with the
     *        fastest start speed, gravity + force and the noise strength, around the shape and
     *        the emitter's path over the last lifetime; clamped by the ground and the wrap box.
     */
    void compute_gpu_bounds_() {
        const ParticleSettings& s = settings;
        const float L = std::max({s.start_lifetime.min, s.start_lifetime.max, 0.0f});
        const float vmax = std::max(std::abs(s.start_speed.min), std::abs(s.start_speed.max));
        const glm::vec3 acc = glm::vec3(0.0f, 0.0f, -9.81f * s.gravity) + s.force;
        const float nz = std::abs(s.noise_strength);
        auto extreme = [L](float v0, float a, bool upper) {
            auto f = [&](float t) { return v0 * t + 0.5f * a * t * t; };
            float best = upper ? std::max(0.0f, f(L)) : std::min(0.0f, f(L));
            if (a != 0.0f) {
                const float ts = -v0 / a;
                if (ts > 0.0f && ts < L) best = upper ? std::max(best, f(ts)) : std::min(best, f(ts));
            }
            return best;
        };
        glm::vec3 lo(0.0f), hi(0.0f);
        float far = 0.0f;
        for (int k = 0; k < 3; ++k) {
            hi[k] = extreme(vmax + s.velocity[k], acc[k] + nz, true);
            lo[k] = extreme(-vmax + s.velocity[k], acc[k] - nz, false);
            far = std::max({far, std::abs(hi[k]), std::abs(lo[k])});
        }
        const ShapeSettings& sh = s.shape;
        float R = 0.0f;
        switch (sh.type) {
            case EmitShape::Point: break;
            case EmitShape::Box:   R = 0.5f * glm::length(sh.box); break;
            case EmitShape::Edge:  R = 0.5f * sh.length; break;
            case EmitShape::Mesh:
                if (surface_ready_) R = std::max(glm::length(surface_->bounds_min()), glm::length(surface_->bounds_max()));
                break;
            default: R = sh.radius; break;
        }
        R += glm::length(sh.offset) + std::abs(sh.normal_offset);
        const float swirl = std::abs(s.orbital) * (R + far) * L + std::abs(s.radial) * L;
        const float sc = std::cbrt(std::abs(glm::determinant(glm::mat3(world_))));
        const glm::vec3 ep(world_[3]);

        float peak = 1.0f;
        for (const auto& k : s.size_over_life.keys()) peak = std::max(peak, k.y);
        float reach = std::max(1.0f, s.look.aspect) * (0.75f + std::abs(s.look.pivot_z));
        if (s.look.mode == render::ParticleRenderMode::Stretched) reach += s.look.stretch_length;
        const float pad = s.start_size.largest() * peak * reach + 0.05f;

        if (simulates_locally_()) {
            const float e = (R + far + swirl + pad) * sc;
            bounds_min_ = ep - glm::vec3(e);
            bounds_max_ = ep + glm::vec3(e);
            return;
        }
        // The emitter's path over the last lifetime (a sample every L / 8 s).
        if (!gpu_trail_.empty() && gpu_trail_.back().first > time_) gpu_trail_.clear();
        if (gpu_trail_.empty() || time_ - gpu_trail_.back().first >= L * 0.125f) gpu_trail_.push_back({time_, ep});
        while (gpu_trail_.size() > 1 && gpu_trail_.front().first < time_ - L - 1e-3f) gpu_trail_.erase(gpu_trail_.begin());
        glm::vec3 pmin = ep, pmax = ep;
        for (const auto& t : gpu_trail_) { pmin = glm::min(pmin, t.second); pmax = glm::max(pmax, t.second); }
        const glm::vec3 r(R * sc + swirl + pad);
        bounds_min_ = pmin + lo - r;
        bounds_max_ = pmax + hi + r;
        if (s.collide) bounds_min_.z = std::max(bounds_min_.z, s.ground_height - pad);
        for (int k = 0; k < 3; ++k) {
            if (s.wrap_box[k] <= 0.0f) continue;
            bounds_min_[k] = std::max(bounds_min_[k], ep[k] - 0.5f * s.wrap_box[k] - pad);
            bounds_max_[k] = std::min(bounds_max_[k], ep[k] + 0.5f * s.wrap_box[k] + pad);
        }
        bounds_max_ = glm::max(bounds_max_, bounds_min_);
    }

    void update_(float dt, const ParallelFor* par) {
        const size_t n = pool_.size();
        if (n == 0 || settings.mode == ParticleMode::Scatter) {
            // A scatter only spins (angular velocity), if asked.
            if (settings.mode == ParticleMode::Scatter) {
                for (size_t i = 0; i < n; ++i) pool_.rot[i] += pool_.rot_vel[i] * dt;
            }
            return;
        }
        const bool local = simulates_locally_();
        const glm::mat3 inv_rot = local ? glm::inverse(glm::mat3(world_)) : glm::mat3(1.0f);
        // Forces are authored in world space; a local simulation feels them rotated into its frame.
        const glm::vec3 gravity = inv_rot * glm::vec3(0.0f, 0.0f, -9.81f * settings.gravity);
        const glm::vec3 force = inv_rot * settings.force;
        const glm::vec3 center = local ? glm::vec3(0.0f) : glm::vec3(world_[3]);
        const glm::vec3 axis = local ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::normalize(glm::mat3(world_)[2] + glm::vec3(0.0f, 0.0f, 1e-8f));
        const float drag_k = 1.0f / (1.0f + std::max(settings.drag, 0.0f) * dt);
        const bool noise = settings.noise_strength != 0.0f;
        const float noise_t = sim_clock_ * settings.noise_scroll;
        const bool collide = settings.collide;
        const float ground = settings.ground_height;
        const float bounce = settings.bounce;
        const float keep_h = 1.0f - std::clamp(settings.collision_friction, 0.0f, 1.0f);
        const std::shared_ptr<const GroundField> field = ground_field;   // swapped between frames only
        const glm::vec3 wrap = local ? glm::vec3(0.0f) : glm::max(settings.wrap_box, glm::vec3(0.0f));
        const bool wrapping = wrap.x > 0.0f || wrap.y > 0.0f || wrap.z > 0.0f;

        run_range(par, n, [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) {
                glm::vec3 p = pool_.pos[i];
                glm::vec3 v = pool_.vel[i];
                glm::vec3 a = gravity + force;
                if (noise) a += turbulence_.sample(local ? glm::vec3(world_ * glm::vec4(p, 1.0f)) : p, noise_t) * settings.noise_strength;
                v = (v + a * dt) * drag_k;
                glm::vec3 dp = (v + settings.velocity) * dt;
                if (settings.orbital != 0.0f || settings.radial != 0.0f) {
                    glm::vec3 r = p - center;
                    r -= axis * glm::dot(r, axis);
                    const float rl = glm::length(r);
                    dp += glm::cross(axis, r) * (settings.orbital * dt);
                    if (rl > 1e-5f) dp += (r / rl) * (settings.radial * dt);
                }
                p += dp;
                float age = pool_.age[i] + dt;
                // Landing is tested before wrapping, so a drop crossing the box's floor where the
                // ground is lands rather than wrapping back up; only a drop that hit nothing wraps.
                const float g0 = collide && !local ? (field ? field->sample(p.x, p.y) : ground) : -1e30f;
                bool wrapped = false;
                if (wrapping && p.z >= g0) {
                    for (int k = 0; k < 3; ++k) {
                        if (wrap[k] <= 0.0f) continue;
                        const float d = p[k] - center[k];
                        const float w = d - wrap[k] * std::floor(d / wrap[k] + 0.5f);
                        if (w != d) { p[k] = center[k] + w; wrapped = true; }
                    }
                }
                if (collide && !local) {
                    const float g = wrapped ? (field ? field->sample(p.x, p.y) : ground) : g0;
                    if (p.z < g && wrapped) {
                        age = k_silent_death_;   // wrapped in under cover: gone, no splash
                    } else if (p.z < g) {
                        if (settings.kill_on_collide) {
                            age = k_collide_death_;
                            p.z = g + 0.02f;     // its death (and any splash) sits just on the surface
                        } else {
                            p.z = g + (g - p.z) * bounce;
                            v.z = std::abs(v.z) * bounce;
                            v.x *= keep_h;
                            v.y *= keep_h;
                        }
                    }
                }
                pool_.pos[i] = p;
                pool_.vel[i] = v;
                pool_.age[i] = age;
                pool_.rot[i] += pool_.rot_vel[i] * dt;
            }
        });
    }

    void compute_bounds_(const ParallelFor* par) {
        const size_t n = pool_.size();
        if (n == 0) {
            bounds_min_ = bounds_max_ = glm::vec3(world_[3]);
            return;
        }
        glm::vec3 mn(1e30f), mx(-1e30f);
        float max_size = 0.0f;
        std::mutex merge;
        run_range(par, n, [&](size_t b, size_t e) {
            glm::vec3 lmn(1e30f), lmx(-1e30f);
            float ls = 0.0f;
            for (size_t i = b; i < e; ++i) {
                lmn = glm::min(lmn, pool_.pos[i]);
                lmx = glm::max(lmx, pool_.pos[i]);
                ls = std::max(ls, pool_.size0[i]);
            }
            std::lock_guard<std::mutex> lock(merge);
            mn = glm::min(mn, lmn);
            mx = glm::max(mx, lmx);
            max_size = std::max(max_size, ls);
        });
        // Size curves can grow a particle past its start size; pad by the curve's peak.
        float peak = 1.0f;
        for (const auto& k : settings.size_over_life.keys()) peak = std::max(peak, k.y);
        // A quad reaches ~0.75 size from its centre (aspect and pivot aside); an instanced mesh
        // reaches its own bounding radius times the size.
        float reach = std::max(1.0f, settings.look.aspect) * (0.75f + std::abs(settings.look.pivot_z));
        if (settings.look.mode == render::ParticleRenderMode::Mesh && proxy_ && proxy_->is_ready()) {
            const auto& m = *proxy_->get_mesh();
            reach = std::max(glm::length(m.bounds_min()), glm::length(m.bounds_max()));
        }
        if (settings.look.mode == render::ParticleRenderMode::Stretched) reach += settings.look.stretch_length;
        const float pad = max_size * peak * reach + 0.05f;
        if (simulates_locally_()) {
            glm::vec3 wmn(1e30f), wmx(-1e30f);
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 corner((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y, (c & 4) ? mx.z : mn.z);
                const glm::vec3 w = glm::vec3(world_ * glm::vec4(corner, 1.0f));
                wmn = glm::min(wmn, w);
                wmx = glm::max(wmx, w);
            }
            const float s = std::cbrt(std::abs(glm::determinant(glm::mat3(world_))));
            mn = wmn; mx = wmx;
            bounds_min_ = mn - glm::vec3(pad * s);
            bounds_max_ = mx + glm::vec3(pad * s);
        } else {
            bounds_min_ = mn - glm::vec3(pad);
            bounds_max_ = mx + glm::vec3(pad);
        }
    }

    Pool pool_;
    Rng rng_{1u};
    TurbulenceField turbulence_;
    std::shared_ptr<const MeshSurface> surface_;
    bool surface_ready_ = false;
    std::unique_ptr<coopa::gfx::engine::components::MeshRenderer> proxy_;

    glm::mat4 world_{1.0f};
    glm::mat4 prev_world_{1.0f};
    bool has_prev_world_ = false;

    float time_ = 0.0f;          ///< Since play() (drives duration, delay, bursts).
    float sim_clock_ = 0.0f;     ///< Monotonic (drives the noise field).
    float emit_accum_ = 0.0f;
    uint32_t pending_emit_ = 0;
    std::vector<PendingAt> pending_at_;
    std::vector<DeathEvent> dead_events_;

    bool playing_ = false;
    /// Death markers in pool_.age (>= any life): killed by a collision, or silently (no sub emitters).
    static constexpr float k_collide_death_ = std::numeric_limits<float>::max() * 0.5f;
    static constexpr float k_silent_death_ = std::numeric_limits<float>::max();
    bool emitting_ = false;
    bool paused_ = false;
    bool started_ = false;
    bool prewarm_pending_ = false;
    bool scattered_ = false;
    bool settings_applied_ = false;

    // GPU simulation (simulation: gpu): the CPU keeps only the clock, emission and bounds.
    bool gpu_active_ = false;
    uint64_t gpu_id_ = 0;
    uint32_t gpu_generation_ = 0;
    uint32_t gpu_alive_ = 0;       ///< Last read-back alive count.
    uint32_t gpu_spawn_ = 0;       ///< Births owed to the next job.
    float gpu_dt_ = 0.0f;          ///< Time owed to the next job.
    glm::mat4 job_prev_world_{1.0f};
    bool has_job_world_ = false;
    std::shared_ptr<const std::vector<glm::vec4>> gpu_tris_;
    const MeshSurface* gpu_tris_src_ = nullptr;
    std::vector<std::pair<float, glm::vec3>> gpu_trail_;

    glm::vec3 bounds_min_{0.0f}, bounds_max_{0.0f};

    // prepare_render() scratch and output.
    std::vector<uint32_t> order_;
    std::vector<float> keys_;
    std::vector<uint32_t> sort_keys_, sort_tmp_keys_, sort_tmp_order_;
    std::vector<render::ParticleInstance> instances_;
    std::vector<glm::mat4> matrices_;
};

} // namespace particles
} // namespace toy

#endif // TOYENGINE_PARTICLES_PARTICLE_SYSTEM_H
