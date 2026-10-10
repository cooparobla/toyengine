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


#include <gfxcoopa/engine/components/mesh_renderer.h>

#include <toyengine/particles/particle_shape.h>
#include <toyengine/render/particle_types.h>

namespace toy {
namespace particles {

/// One range body: handles indices [begin, end).
using RangeFn = std::function<void(std::size_t begin, std::size_t end)>;
/// Runs `fn` over [0, n), each index exactly once, possibly split across threads; returns when done.
using ParallelFor = std::function<void(std::size_t n, const RangeFn& fn)>;

/** @brief `fn` over [0, n): through `par` when given, else inline. */
void run_range(const ParallelFor* par, std::size_t n, const RangeFn& fn);

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
    bool splashes(float x, float y) const;

    /** @brief The surface normal under (x, y); up where unknown. */
    glm::vec3 normal(float x, float y) const;

    float sample(float x, float y) const;
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
    void set_shape_surface(std::shared_ptr<const MeshSurface> surface);
    /** @brief Hand over a surface built by hand (tests, procedural emitters). */
    void set_shape_surface(MeshSurface surface) { set_shape_surface(std::make_shared<const MeshSurface>(std::move(surface))); }
    const MeshSurface* shape_surface() const { return surface_.get(); }
    bool shape_surface_ready() const { return surface_ready_; }

    /** @brief Mesh render mode's mesh + material, as a MeshRenderer that is never attached. */
    coopa::gfx::engine::components::MeshRenderer& mesh_proxy();
    bool has_mesh_proxy() const { return proxy_ != nullptr; }

    /** @brief The optional texture (albedo slot) for `sprite: texture`. */
    coopa::gfx::engine::components::PBRMaterial texture_material;
    bool has_texture() const { return texture_material.has_albedo_map(); }

    /** @brief Re-bakes settings-derived state (noise field); call after editing `settings` by hand. */
    void apply_settings();

    void start() override { init(); }

    /**
     * @brief One-time setup: seed, emitter mesh, mesh proxy owner, play_on_start. Idempotent --
     *        Component::start() calls it in play mode, and ParticleSimulationSystem calls it
     *        for systems in a scene that is only being edited (which never start()s).
     */
    void init();
    bool initialized() const { return started_; }

    // ------------------------------------------------------------------ playback

    void play();
    /** @brief Stops emitting; live particles finish their lives unless `clear_particles`. */
    void stop(bool clear_particles = false);
    void pause(bool paused) { paused_ = paused; }
    bool paused() const { return paused_; }
    void clear();
    /** @brief Restart from t = 0 (clears particles and re-arms bursts). */
    void restart();
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
    bool is_alive() const;
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
    void step(float dt, const glm::mat4& world, const ParallelFor* par = nullptr);

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
    void prepare_render(const ParticleView& view, const ParallelFor* par = nullptr);

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
    std::string gpu_fallback_reason(bool is_sub_target) const;
    /** @brief Set by ParticleSimulationSystem each step; switching either way clears the pool. */
    void set_gpu_active(bool on);
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
    bool make_gpu_job(const ParticleView& view, render::GpuParticleJob& job);

    /// Raw pool access for tests and tools.
    struct Pool {
        std::vector<glm::vec3> pos, vel;
        std::vector<float>     age, life, size0, rot, rot_vel;
        std::vector<glm::vec4> color0;
        std::vector<glm::quat> orient;
        std::vector<uint32_t>  seed;

        size_t size() const { return pos.size(); }
        bool empty() const { return pos.empty(); }
        void clear();
        void reserve(size_t n);
        void swap_remove(size_t i);
    };
    const Pool& pool() const { return pool_; }

private:
    struct PendingAt { glm::vec3 pos; glm::vec3 vel; glm::vec3 normal; };

    uint32_t effective_seed_() const;

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
    void radix_sort_order_();

    float life_t_(uint32_t i, bool scatter) const;

    static glm::mat3 orthonormal_(const glm::mat3& m);

    /** @brief One shape sample in emitter-local space. `u` drives the mesh CDF (stratified for even). */
    EmitSample sample_shape_(float u);

    bool can_spawn_() const {
        return settings.shape.type != EmitShape::Mesh || surface_ready_;
    }

    /**
     * @brief Appends one particle. `frac` in [0, 1] is how far back in this frame it was born (1 =
     *        at the frame's start), used to place it along the emitter's path and pre-age it.
     */
    void spawn_(float dt, float frac, const EmitSample* forced = nullptr, const glm::vec3* forced_vel = nullptr,
                float u = -1.0f);

    /** @brief How many particles the rate, distance and bursts ask for over [t0, t1). */
    uint32_t emission_count_(float t0, float t1);

    void step_once_(float dt, const ParallelFor* par, bool emit);

    /** @brief A GPU system's CPU half: the clock, the emission count (rate, distance, bursts --
     *         the same emission_count_() as the CPU path) and conservative bounds. */
    void step_gpu_(float dt);

    /**
     * @brief Bounds a GPU system's particles cannot leave, from the settings alone (nothing is
     *        read back): per axis, the extremes of v0 t + a t^2 / 2 over a lifetime with the
     *        fastest start speed, gravity + force and the noise strength, around the shape and
     *        the emitter's path over the last lifetime; clamped by the ground and the wrap box.
     */
    void compute_gpu_bounds_();

    void update_(float dt, const ParallelFor* par);

    void compute_bounds_(const ParallelFor* par);

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
