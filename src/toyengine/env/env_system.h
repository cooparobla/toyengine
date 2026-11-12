/**
 * @file env_system.h
 * @brief `EnvSystem` (order 45) -- gathers the scene-global weather and every local influence
 *        into one `EnvField` snapshot a frame, which everything that reacts to the weather reads.
 *
 * Environment = one global contributor (toyengine/weather/, scene-wide: the clock, the
 * condition, the mean wind) plus any number of local contributors (toyengine/env/: a rotor's
 * downwash, a vent, a sword swing). This system folds them into a single field so no consumer
 * has to know which kind it is reading.
 *
 * Order **45** is after Weather (40) and before every consumer -- Water (90), Wind (95),
 * Physics (100), the Behaviour walk (200), Particles (360), Snow (370) and the renderer's
 * surface-world upload -- so all of them answer "what is the wind at p" identically for the
 * whole frame.
 *
 * ### The position contract
 *
 * A source's position comes from its object's world matrix, which TransformResolve finalises
 * at order **350** -- after this gather. A source on a physics-driven or animated object is
 * therefore reading last frame's resolved transform, extrapolated one frame from the frame
 * before it (which cancels the lag for anything moving at a steady speed). This is deliberate:
 * ONE snapshot per frame is the property that makes the field testable, and a second gather
 * after TransformResolve would leave the grass and the GPU on frame N while cloth, water and
 * rigidbody drag were still on frame N-1 -- a visibly wrong wake, and far harder to debug than
 * a third of a metre of lag on a 20 m/s helicopter.
 *
 * Contrast `SnowDeformer` (order 370), which stamps where it is drawn because it runs after
 * TransformResolve and has no CPU consumers downstream of it.
 *
 * ### Reading it
 *
 * @code
 * const toy::env::EnvField& f = toy::env::current_field();   // any component, any time
 * glm::vec3 w = toy::env::wind_at(f, position);              // weather + gusts + locals
 * float wet  = toy::env::wetness_at(f, position);
 *
 * toy::env::EnvPulse p;                      // a one-shot gust: a swing, a blast
 * p.source.position = blade_tip;
 * p.source.radius = 2.5f;
 * p.source.mode = toy::env::EnvMode::Radial;
 * p.source.outward = 12.0f;
 * toy::env::emit(scene, p);
 * @endcode
 *
 * See toyengine/env/README.md, and toyengine/weather/README.md's "Wind" table for the
 * consumers.
 */

#ifndef TOYENGINE_ENV_ENV_SYSTEM_H
#define TOYENGINE_ENV_ENV_SYSTEM_H

#include <memory>
#include <unordered_map>
#include <vector>

#include <coopa/event/signal.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_system.h>

#include <toyengine/env/env_field.h>
#include <toyengine/env/env_influence.h>

namespace toy {
namespace env {

/** @brief Order the system runs at: after Weather (40), before every consumer of the field. */
inline constexpr int k_env_system_order = 45;

/**
 * @class EnvSystem
 * @brief See the file doc.
 */
class EnvSystem : public coopa::scene::ISceneSystem {
public:
    ~EnvSystem() override;

    const char* system_name() const override { return "Environment"; }
    /// Runs in the editor so local influences preview without entering Play, as the weather does.
    bool runs_in_edit_mode() const override { return true; }

    void execute(coopa::scene::Scene& scene, const coopa::scene::FrameContext& ctx) override;

    /** @brief This frame's field. Valid until the next execute(). */
    const EnvField& field() const { return field_; }

    /**
     * @brief An immutable snapshot of this frame's field, safe to read from job threads.
     *
     * Particle simulation runs on jobs and rigidbody drag runs inside the physics substep;
     * both hold one of these for the frame rather than reading `field()` under a lock.
     */
    const std::shared_ptr<const EnvField>& snapshot() const { return snapshot_; }

    /**
     * @brief Adds a one-shot influence. It is folded in from the next gather and dropped once
     *        its envelope expires.
     *
     * @param pulse The pulse.
     */
    void emit(const EnvPulse& pulse);


    /** @brief Live one-shot pulses (diagnostics / tests). */
    std::size_t live_pulse_count() const { return pulses_.size(); }

    /** @brief How many sources the last gather dropped because the field was full. */
    int dropped_last_frame() const { return dropped_; }

    /** @brief Fires for each pulse emitted -- audio, gameplay, screen shake. */
    coopa::event::Signal<const EnvPulse&> on_pulse;

    /** @brief The system of the scene that updated most recently, or null. */
    static EnvSystem* active() { return active_slot_(); }

private:
    struct LivePulse {
        EnvPulse pulse;
        float age = 0.0f;
    };

    static EnvSystem*& active_slot_();

    EnvField field_;
    std::shared_ptr<const EnvField> snapshot_;
    std::vector<LivePulse> pulses_;
    std::unordered_map<const EnvInfluence*, glm::vec3> prev_pos_;
    std::vector<EnvSource> gathered_;
    int dropped_ = 0;
    int reported_drop_ = -1;   ///< Last drop count logged, so the message prints on change only.
};

/** @brief The scene's system, or null. */
inline EnvSystem* find_env(const coopa::scene::Scene& scene) {
    return dynamic_cast<EnvSystem*>(scene.find_system("Environment"));
}

/**
 * @brief Installs the system on `scene`, or returns the one already there.
 *
 * @param scene The scene.
 * @param order The update order; the default is what every consumer assumes.
 * @return EnvSystem* The system.
 */
EnvSystem* install_env_system(coopa::scene::Scene& scene, int order = k_env_system_order);

/**
 * @brief The active scene's environment field. With no system it is a calm, dry, 18 C world.
 *
 * Read-only, for gameplay that has no `Scene&` to hand -- the same convenience
 * toy::weather::current() gives.
 *
 * @return const EnvField& The field.
 */
const EnvField& current_field();

/**
 * @brief Emits a one-shot influence into `scene`.
 *
 * @param scene The scene.
 * @param pulse The pulse.
 * @return bool False when the scene has no EnvSystem, so nothing took it.
 */
bool emit(coopa::scene::Scene& scene, const EnvPulse& pulse);

/**
 * @brief Builds the field a scene with no EnvSystem would have: the weather alone.
 *
 * The fallback for consumers that must keep working when the system is absent (standalone
 * runners and tests that install one system in isolation).
 *
 * @param scene The scene.
 * @return EnvField The weather-only field.
 */
EnvField weather_only_field(const coopa::scene::Scene& scene);

} // namespace env
} // namespace toy

#endif // TOYENGINE_ENV_ENV_SYSTEM_H
