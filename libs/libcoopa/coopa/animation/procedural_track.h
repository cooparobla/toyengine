/**
 * @file procedural_track.h
 * @brief Formula-driven animation tracks: parameters, live-input snapshots, and
 *        a string-keyed evaluator registry mirroring AnimatedPropertyRegistry.
 */

#ifndef COOPA_ANIMATION_PROCEDURAL_TRACK_H
#define COOPA_ANIMATION_PROCEDURAL_TRACK_H

#include <glm/glm.hpp>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace coopa {
namespace anim {

/**
 * @struct ProceduralParams
 * @brief Immutable, clip-authored parameters for one procedural track.
 *
 * Part of the shared, const AnimationClip payload — sampled from worker
 * threads by many Animators at once. Never mutated after the clip is parsed.
 */
struct ProceduralParams {
    std::unordered_map<std::string, float>     scalars;
    std::unordered_map<std::string, glm::vec4> vectors;

    float scalar(const std::string& key, float fallback) const {
        auto it = scalars.find(key);
        return it != scalars.end() ? it->second : fallback;
    }

    glm::vec4 vector(const std::string& key, const glm::vec4& fallback) const {
        auto it = vectors.find(key);
        return it != vectors.end() ? it->second : fallback;
    }
};

/**
 * @struct ProceduralInputs
 * @brief Live scene values snapshotted on the MAIN THREAD, read-only inside evaluate_().
 *
 * This is the one piece of per-track runtime state a procedural evaluator may
 * read that is NOT part of the shared const clip. It must never be written
 * from inside an evaluator (which may run on a worker thread) — only
 * Animator::advance_() writes it, once per frame, before any evaluation
 * starts. Kept generic (not "orbit_center") so future evaluator types can use
 * it without inventing another per-track snapshot slot.
 */
struct ProceduralInputs {
    glm::vec3 target_position{0.0f};
    bool      target_valid = false;
};

/**
 * @brief A pure function of (params, inputs, time) producing up to 4 floats.
 *
 * MUST be stateless and reentrant: evaluators run inside Animator::evaluate_()
 * on worker threads, the same constraint AnimationCurve::sample() carries. Do
 * not use a `static` local for memoization — that would be a data race across
 * concurrently-evaluating Animators. `out` arrives pre-initialized to
 * {0, 0, 0, 1}, matching Keyframe::value's identity default.
 */
using ProceduralEvaluator = std::function<void(const ProceduralParams& params,
                                               const ProceduralInputs& inputs,
                                               float time, uint8_t component_count,
                                               float* out)>;

/**
 * @struct ProceduralTrack
 * @brief Clip-authored data for one procedural track: which evaluator, and its parameters.
 *
 * Pure data, part of the shared const AnimationClip — safe to read from
 * worker threads. target_object is resolved to a SceneObject* once at an
 * Animator's bind time (never here), and that object's position is
 * snapshotted into a per-Animator ProceduralInputs each frame in advance_() —
 * see animator.h. This struct never touches the scene.
 */
struct ProceduralTrack {
    std::string      type; ///< Registry key: "orbit", "sine", "spin", "constant", or a consumer-registered type.
    ProceduralParams params;
    std::string      target_object; ///< Optional; empty means no live-position input.
};

/**
 * @class ProceduralTrackRegistry
 * @brief String-keyed registry of procedural evaluators.
 *
 * Function-local static, mirroring AnimatedPropertyRegistry::instance() and
 * SceneLoader's parser registry — populated from one entry point per repo, so
 * gfxcoopa/uicoopa can add procedural types (a noise field, a spring, ...)
 * without libcoopa ever naming them.
 */
class ProceduralTrackRegistry {
public:
    static ProceduralTrackRegistry& instance() {
        static ProceduralTrackRegistry registry;
        registry.add_builtins_if_empty_();
        return registry;
    }

    void add(const std::string& type, ProceduralEvaluator fn) {
        evaluators_[type] = std::move(fn);
    }

    const ProceduralEvaluator* find(const std::string& type) const {
        auto it = evaluators_.find(type);
        return it != evaluators_.end() ? &it->second : nullptr;
    }

    /** @brief Test hook: clears everything, then re-adds the built-ins. */
    void clear() {
        evaluators_.clear();
        add_builtins_if_empty_();
    }

private:
    void add_builtins_if_empty_() {
        if (!evaluators_.empty()) return;

        // constant: out[c] = value[c]. The trivial evaluator; mostly useful
        // as a crossfade endpoint or a test fixture.
        evaluators_["constant"] = [](const ProceduralParams& p, const ProceduralInputs&,
                                     float, uint8_t count, float* out) {
            glm::vec4 v = p.vector("value", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            for (uint8_t c = 0; c < count; ++c) out[c] = v[c];
        };

        // orbit: theta = initial_angle + speed*t; out = center + (r*cos, r*sin, height).
        // When target_object was set and resolved, `inputs.target_position`
        // replaces `center` — snapshotted on the main thread each frame by
        // Animator::advance_(), never read from the scene here.
        evaluators_["orbit"] = [](const ProceduralParams& p, const ProceduralInputs& inputs,
                                  float time, uint8_t count, float* out) {
            glm::vec3 center = inputs.target_valid
                ? inputs.target_position
                : glm::vec3(p.vector("center", glm::vec4(0.0f)));
            float radius        = p.scalar("radius", 1.0f);
            float speed         = p.scalar("speed", 1.0f);
            float height        = p.scalar("height", 0.0f);
            float initial_angle = p.scalar("initial_angle", 0.0f);
            float theta = initial_angle + speed * time;
            float result[4] = {
                center.x + radius * std::cos(theta),
                center.y + radius * std::sin(theta),
                center.z + height,
                1.0f,
            };
            for (uint8_t c = 0; c < count; ++c) out[c] = result[c];
        };

        // sine: out[c] = offset[c] + amplitude[c] * sin(2*pi*frequency[c]*t + phase[c]).
        // The generic oscillator — covers most idle UI motion (float, breathe, sway).
        evaluators_["sine"] = [](const ProceduralParams& p, const ProceduralInputs&,
                                 float time, uint8_t count, float* out) {
            glm::vec4 amplitude = p.vector("amplitude", glm::vec4(1.0f, 1.0f, 1.0f, 0.0f));
            glm::vec4 frequency = p.vector("frequency", glm::vec4(1.0f));
            glm::vec4 phase     = p.vector("phase", glm::vec4(0.0f));
            glm::vec4 offset    = p.vector("offset", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            constexpr float k_two_pi = 6.28318530718f;
            for (uint8_t c = 0; c < count; ++c) {
                out[c] = offset[c] + amplitude[c] * std::sin(k_two_pi * frequency[c] * time + phase[c]);
            }
        };

        // spin: out[c] = offset[c] + rate[c]*t. Unbounded linear ramp — the one
        // thing a finite keyframed curve structurally cannot express (rotate
        // forever, scroll forever) without looping or clamping.
        evaluators_["spin"] = [](const ProceduralParams& p, const ProceduralInputs&,
                                 float time, uint8_t count, float* out) {
            glm::vec4 rate   = p.vector("rate", glm::vec4(0.0f, 0.0f, 0.0f, 0.0f));
            glm::vec4 offset = p.vector("offset", glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            for (uint8_t c = 0; c < count; ++c) out[c] = offset[c] + rate[c] * time;
        };
    }

    std::unordered_map<std::string, ProceduralEvaluator> evaluators_;
};

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_PROCEDURAL_TRACK_H
