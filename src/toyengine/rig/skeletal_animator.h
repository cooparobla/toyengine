/**
 * @file skeletal_animator.h
 * @brief Component that plays SkeletalClips against named states onto a sibling Rig's pose,
 *        with crossfade blending.
 */

#ifndef TOYENGINE_RIG_SKELETAL_ANIMATOR_H
#define TOYENGINE_RIG_SKELETAL_ANIMATOR_H

#include <toyengine/anim/root_motion.h>
#include <coopa/asset/asset_handle.h>
#include <coopa/event/event_bus.h>
#include <coopa/event/signal.h>
#include <toyengine/rig/rig_component.h>
#include <toyengine/rig/skeletal_clip.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace toy {
namespace rig {

class SkeletalAnimationSystem; // Forward declaration — SkeletalAnimator grants it friend access.

/**
 * @struct SkeletalAnimatorState
 * @brief One named entry on a SkeletalAnimator: a clip (inline or asset-loaded) plus
 *        per-state overrides. Mirrors toy::anim::AnimatorState exactly, typed to
 *        SkeletalClip instead of AnimationClip.
 */
struct SkeletalAnimatorState {
    std::string name;

    std::shared_ptr<SkeletalClip> inline_clip;
    coopa::asset::AssetHandle<SkeletalClip> asset_clip;

    bool                   has_wrap_override = false;
    toy::anim::WrapMode  wrap_override = toy::anim::WrapMode::Loop;
    float                  speed = 1.0f;

    const SkeletalClip* resolve() const {
        if (inline_clip) return inline_clip.get();
        if (asset_clip.is_loaded()) return asset_clip.get();
        return nullptr;
    }
    uint32_t revision() const { return asset_clip.is_valid() ? asset_clip.revision() : 0; }
    toy::anim::WrapMode effective_wrap(const SkeletalClip& clip) const {
        return has_wrap_override ? wrap_override : clip.wrap;
    }
};

/**
 * @class SkeletalAnimator
 * @brief Plays named SkeletalAnimatorStates, crossfading between them, writing directly into
 *        the pose channels of the Rig on the SAME object.
 *
 * Unlike toy::anim::Animator, a track's target is not resolved through a generic
 * (SceneObject, Component, property) binding — it is a bone NAME, looked up once as an
 * index into the sibling Rig at bind time (SkeletalAnimator::rebind()). There is therefore
 * no AnimatedPropertyRegistry involved at all, and no cross-Animator dedup problem: a Rig's
 * pose buffer belongs to exactly one rig, and at most one SkeletalAnimator is expected to
 * drive it (a second one driving the same Rig would simply overwrite the first's channels
 * in component order, same caveat toy::anim::AnimationSystem::find_conflicts() exists
 * for on the property side).
 *
 * Rotation blends by true slerp (see sample_rotation() and this class's apply_()), not the
 * componentwise-lerp-then-normalize toy::anim's `rotation_quat` AnimatedProperty uses —
 * hemisphere correctness never depends on how a clip's keys were authored.
 *
 * Deliberately has no worker-safe evaluate_() phase the way Animator does: it writes
 * straight into Rig::pose(), owned by a sibling component, rather than through a generic
 * get/set pair, so there is no natural sample/apply split to dispatch across threads. A
 * scene has a handful of rigs, not hundreds of UI widgets — unlike toy::anim::Animator,
 * batching this across a JobEngine is not expected to matter.
 */
class SkeletalAnimator : public coopa::scene::Component {
public:
    SkeletalAnimator() { bump_generation_(); }
    ~SkeletalAnimator() override { bump_generation_(); }

    std::string type_name() const override { return "SkeletalAnimator"; }

    /** @brief Process-wide count of SkeletalAnimators created or destroyed so far — see toy::anim::Animator::generation(). */
    static uint64_t generation() { return generation_counter_().load(std::memory_order_acquire); }

    /** @brief Name of the state to play automatically once bound. */
    std::string auto_play;
    /** @brief Convenience default duration for a caller that triggers crossfade() without picking its own. */
    float default_crossfade = 0.0f;
    /** @brief See toy::anim::Animator::apply_root_motion — identical semantics, bone-addressed. */
    bool apply_root_motion = false;

    /** @brief Fired once per play()/crossfade() when a WrapMode::Once state reaches its end. */
    coopa::event::Signal<const std::string&> on_state_finished;
    /** @brief Fired for every clip event the current state's playhead crosses. See toy::anim::Animator::on_event's doc — identical rules. */
    coopa::event::Signal<const toy::anim::AnimationEvent&> on_event;

    /**
     * @brief Finds the sibling Rig, binds and starts auto_play, if set.
     *
     * Does NOT warn about a missing SkeletalAnimationSystem, for the same reason
     * toy::anim::Animator::start() doesn't — see that method's doc.
     */
    void start() override {
        _rig = owner ? owner->get_component<Rig>() : nullptr;
        if (!_rig) {
            std::cerr << "[SkeletalAnimator] '" << (owner ? owner->name() : std::string("?"))
                      << "': no sibling Rig component -- a SkeletalAnimator must share its object with a Rig" << std::endl;
        }
        rebind();
        if (!auto_play.empty()) play(auto_play);
    }

    // --- Building states ---

    SkeletalAnimatorState* add_state(const std::string& name, std::shared_ptr<SkeletalClip> clip) {
        auto state = std::make_unique<SkeletalAnimatorState>();
        state->name = name;
        state->inline_clip = std::move(clip);
        SkeletalAnimatorState* raw = state.get();
        _states.push_back(std::move(state));
        _bound = false;
        return raw;
    }

    SkeletalAnimatorState* add_state(const std::string& name, coopa::asset::AssetHandle<SkeletalClip> clip) {
        auto state = std::make_unique<SkeletalAnimatorState>();
        state->name = name;
        state->asset_clip = std::move(clip);
        SkeletalAnimatorState* raw = state.get();
        _states.push_back(std::move(state));
        _bound = false;
        return raw;
    }

    SkeletalAnimatorState* find_state(const std::string& name) const {
        for (const auto& s : _states) {
            if (s->name == name) return s.get();
        }
        return nullptr;
    }

    // --- Playback ---

    void play(const std::string& state, float start_time = 0.0f) {
        SkeletalAnimatorState* s = find_state(state);
        if (!s) {
            std::cerr << "[SkeletalAnimator] play(): unknown state '" << state << "'" << std::endl;
            return;
        }
        _current = Layer{};
        _current.state = s;
        _current.time = start_time;
        _current.weight = 1.0f;
        _previous = Layer{};
        _crossfading = false;
        _playing = true;
        ensure_state_bound_(s);
    }

    void crossfade(const std::string& state, float duration) {
        if (!_current.state || duration <= 0.0f) {
            play(state);
            return;
        }
        SkeletalAnimatorState* s = find_state(state);
        if (!s) {
            std::cerr << "[SkeletalAnimator] crossfade(): unknown state '" << state << "'" << std::endl;
            return;
        }
        _previous = _current;
        _previous.weight = 1.0f;
        _current = Layer{};
        _current.state = s;
        _current.time = 0.0f;
        _current.weight = 0.0f;
        _crossfading = true;
        _crossfade_elapsed = 0.0f;
        _crossfade_duration = duration;
        _playing = true;
        ensure_state_bound_(s);
    }

    void stop() { _playing = false; }

    /** @brief Stops AND drops the current state — see toy::anim::Animator::clear()'s doc. */
    void clear() {
        _current = Layer{};
        _previous = Layer{};
        _crossfading = false;
        _playing = false;
    }

    /** @brief Seeks the current state to `time` and writes its pose immediately, without advancing playback. */
    void sample_at(float time) {
        if (!_current.state || !_rig) return;
        ensure_state_bound_(_current.state);
        const SkeletalClip* clip = _current.state->resolve();
        if (!clip) return;
        bool unused_finished = false;
        _current.time = time;
        _current.sample_time = wrap_time_(time, clip->effective_length(), _current.state->effective_wrap(*clip), unused_finished);
        apply_layer_only_(_current);
    }

    void set_speed(float s) { _speed = s; }
    float speed() const { return _speed; }

    void set_time(float t) { _current.time = t; }
    float time() const { return _current.time; }

    float normalized_time() const {
        if (!_current.state) return 0.0f;
        const SkeletalClip* clip = _current.state->resolve();
        if (!clip) return 0.0f;
        const float len = clip->effective_length();
        return len > 0.0f ? _current.time / len : 0.0f;
    }

    bool is_playing() const { return _playing && _current.state != nullptr; }

    const toy::anim::RootMotionDelta& root_motion_delta() const { return _root_delta; }

    const std::string& current_state() const {
        static const std::string empty;
        return _current.state ? _current.state->name : empty;
    }

    /** @brief Re-resolves every track's bone name against the sibling Rig. See toy::anim::Animator::rebind()'s doc. */
    void rebind() {
        for (auto& state : _states) rebind_state_(*state);
        _bound = true;
    }

private:
    friend class SkeletalAnimationSystem;

    static std::atomic<uint64_t>& generation_counter_() {
        static std::atomic<uint64_t> counter{0};
        return counter;
    }
    static void bump_generation_() { generation_counter_().fetch_add(1, std::memory_order_acq_rel); }

    /** @brief Per-track resolved state, parallel to one SkeletalClip::bones entry. */
    struct BoneRef {
        int  bone_index = -1;
        bool root_translation[3] = {false, false, false}; ///< Which position channels are extracted as root motion.
        bool root_yaw = false;
    };
    struct StateBinding {
        std::vector<BoneRef> bone_refs;
        uint32_t             bound_revision = 0;
        bool                 has_clip = false;
    };
    struct Layer {
        SkeletalAnimatorState* state = nullptr;
        float time = 0.0f;
        float sample_time = 0.0f;
        float weight = 1.0f;
        bool  finished_fired = false;
    };
    struct PendingEvent {
        const toy::anim::AnimationEvent* event = nullptr;
        const std::string* state = nullptr;
    };

    void ensure_state_bound_(SkeletalAnimatorState* state) {
        if (!state) return;
        auto it = _state_bindings.find(state);
        if (it == _state_bindings.end() || !it->second.has_clip || it->second.bound_revision != state->revision()) {
            rebind_state_(*state);
        }
    }

    void rebind_state_(SkeletalAnimatorState& state) {
        const SkeletalClip* clip = state.resolve();
        StateBinding& sb = _state_bindings[&state];
        sb.bone_refs.clear();
        sb.has_clip = (clip != nullptr);
        if (!clip || !_rig) return;
        sb.bound_revision = state.revision();
        sb.bone_refs.resize(clip->bones.size());
        for (size_t i = 0; i < clip->bones.size(); ++i) {
            BoneRef& ref = sb.bone_refs[i];
            ref.bone_index = _rig->find_bone(clip->bones[i].bone);
            if (ref.bone_index < 0) {
                std::cerr << "[SkeletalAnimator] '" << (owner ? owner->name() : std::string("?"))
                          << "': no bone named '" << clip->bones[i].bone << "' on the rig" << std::endl;
                continue;
            }
            if (clip->root_motion.enabled() && clip->bones[i].bone == clip->root_motion.bone) {
                if (clip->root_motion.translation != toy::anim::RootMotionTranslation::None) {
                    ref.root_translation[0] = ref.root_translation[1] = true;
                    ref.root_translation[2] = clip->root_motion.translation == toy::anim::RootMotionTranslation::XYZ;
                }
                ref.root_yaw = clip->root_motion.yaw;
            }
        }
    }

    /** @brief Identical algorithm to toy::anim::Animator::wrap_time_() — see that method's doc. */
    static float wrap_time_(float t, float length, toy::anim::WrapMode wrap, bool& out_just_finished) {
        out_just_finished = false;
        if (length <= 0.0f) return t;
        switch (wrap) {
            case toy::anim::WrapMode::Once:
                if (t >= length) { out_just_finished = true; return length; }
                if (t < 0.0f) return 0.0f;
                return t;
            case toy::anim::WrapMode::PingPong: {
                const float period = length * 2.0f;
                float m = std::fmod(t, period);
                if (m < 0.0f) m += period;
                return (m > length) ? (period - m) : m;
            }
            case toy::anim::WrapMode::Loop:
            default: {
                float m = std::fmod(t, length);
                if (m < 0.0f) m += length;
                return m;
            }
        }
    }

    /** @brief Identical algorithm to toy::anim::Animator::for_each_sample_run_() — see that method's doc. */
    template <typename F>
    static void for_each_sample_run_(float t0, float t1, float length, toy::anim::WrapMode wrap, F&& f) {
        if (t0 == t1) return;
        if (length <= 0.0f) { f(t0, t1); return; }
        if (wrap == toy::anim::WrapMode::Once) {
            const float a = std::clamp(t0, 0.0f, length), b = std::clamp(t1, 0.0f, length);
            if (a != b) f(a, b);
            return;
        }
        const bool forward = t1 > t0;
        const float lo_t = forward ? t0 : t1, hi_t = forward ? t1 : t0;
        long long k_min = static_cast<long long>(std::floor(lo_t / length));
        long long k_max = static_cast<long long>(std::ceil(hi_t / length)) - 1;
        constexpr long long k_max_runs = 1024;
        if (k_max - k_min >= k_max_runs) {
            if (forward) k_min = k_max - (k_max_runs - 1);
            else k_max = k_min + (k_max_runs - 1);
        }
        for (long long i = 0; i <= k_max - k_min; ++i) {
            const long long k = forward ? k_min + i : k_max - i;
            const float base = static_cast<float>(k) * length;
            const float lo = std::max(lo_t - base, 0.0f), hi = std::min(hi_t - base, length);
            if (!(hi > lo)) continue;
            float from = forward ? lo : hi, to = forward ? hi : lo;
            if (wrap == toy::anim::WrapMode::PingPong && (k & 1)) { from = length - from; to = length - to; }
            f(from, to);
        }
    }

    /** @brief Identical event-firing rules to toy::anim::Animator::collect_events_() — see that method's doc. */
    void collect_events_(const SkeletalClip& clip, toy::anim::WrapMode wrap, float from, float to, const std::string& state) {
        const float len = clip.effective_length();
        const size_t first = _pending_events.size();
        for (const toy::anim::AnimationEvent& ev : clip.events) {
            bool hit;
            if (to > from) hit = (ev.time >= from && ev.time < to) ||
                                 (wrap != toy::anim::WrapMode::PingPong && len > 0.0f && to >= len && ev.time >= len && from < len);
            else hit = ev.time > to && ev.time <= from;
            if (hit) _pending_events.push_back(PendingEvent{&ev, &state});
        }
        if (to < from) std::reverse(_pending_events.begin() + static_cast<std::ptrdiff_t>(first), _pending_events.end());
    }

    /** @brief The root bone's extracted channels at sample time `s`. */
    void sample_root_(const SkeletalClip& clip, const StateBinding& sb, float s, glm::vec3& pos, float& yaw_deg) const {
        pos = glm::vec3(0.0f);
        yaw_deg = 0.0f;
        for (size_t i = 0; i < clip.bones.size() && i < sb.bone_refs.size(); ++i) {
            const BoneRef& ref = sb.bone_refs[i];
            if (ref.bone_index < 0) continue;
            if (!ref.root_translation[0] && !ref.root_translation[1] && !ref.root_translation[2] && !ref.root_yaw) continue;
            const SkeletalTrack& track = clip.bones[i];
            if ((ref.root_translation[0] || ref.root_translation[1] || ref.root_translation[2]) && !track.location.empty()) {
                float tmp[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                track.location.sample(s, 3, tmp);
                for (int c = 0; c < 3; ++c) {
                    if (ref.root_translation[c]) pos[c] = tmp[c];
                }
            }
            if (ref.root_yaw && !track.rotation.empty()) {
                const glm::quat q = sample_rotation(track.rotation, s);
                yaw_deg = glm::degrees(std::atan2(2.0f * (q.w * q.z + q.x * q.y), q.w * q.w + q.x * q.x - q.y * q.y - q.z * q.z));
            }
        }
    }

    /** @brief The root motion a sample-time run moves through — see toy::anim::Animator::root_run_delta_()'s doc. */
    toy::anim::RootMotionDelta root_run_delta_(const SkeletalClip& clip, const StateBinding& sb, float from, float to) const {
        glm::vec3 p0, p1;
        float y0, y1;
        sample_root_(clip, sb, from, p0, y0);
        sample_root_(clip, sb, to, p1, y1);
        toy::anim::RootMotionDelta d;
        d.yaw_deg = std::remainder(y1 - y0, 360.0f);
        d.translation = p1 - p0;
        if (y0 != 0.0f) {
            const float r = glm::radians(-y0), c = std::cos(r), sn = std::sin(r);
            d.translation = glm::vec3(c * d.translation.x - sn * d.translation.y,
                                      sn * d.translation.x + c * d.translation.y, d.translation.z);
        }
        return d;
    }

    toy::anim::RootMotionDelta advance_layer_(Layer& layer, float dt, bool fire_events) {
        toy::anim::RootMotionDelta delta;
        if (!layer.state) return delta;
        const SkeletalClip* clip = layer.state->resolve();
        if (!clip) return delta;
        const float t0 = layer.time;
        layer.time += dt * _speed * layer.state->speed;
        const float length = clip->effective_length();
        const toy::anim::WrapMode wrap = layer.state->effective_wrap(*clip);
        bool just_finished = false;
        layer.sample_time = wrap_time_(layer.time, length, wrap, just_finished);

        const bool want_events = fire_events && !clip->events.empty();
        const StateBinding* sb = nullptr;
        if (clip->root_motion.enabled()) {
            auto it = _state_bindings.find(layer.state);
            if (it != _state_bindings.end()) sb = &it->second;
        }
        if (want_events || sb) {
            for_each_sample_run_(t0, layer.time, length, wrap, [&](float from, float to) {
                if (want_events) collect_events_(*clip, wrap, from, to, layer.state->name);
                if (sb) {
                    const toy::anim::RootMotionDelta d = root_run_delta_(*clip, *sb, from, to);
                    const float r = glm::radians(delta.yaw_deg), c = std::cos(r), sn = std::sin(r);
                    delta.translation += glm::vec3(c * d.translation.x - sn * d.translation.y,
                                                   sn * d.translation.x + c * d.translation.y, d.translation.z);
                    delta.yaw_deg += d.yaw_deg;
                }
            });
        }

        if (just_finished && !layer.finished_fired) {
            layer.finished_fired = true;
            on_state_finished.emit(layer.state->name);
        } else if (!just_finished) {
            layer.finished_fired = false;
        }
        return delta;
    }

    void dispatch_events_() {
        if (_pending_events.empty()) return;
        std::vector<PendingEvent> events;
        events.swap(_pending_events);
        for (const PendingEvent& pe : events) {
            const toy::anim::AnimationEvent ev = *pe.event;
            const std::string state = *pe.state;
            on_event.emit(ev);
            if (scene && owner) {
                coopa::event::EventArgs args;
                args.set("name", ev.name).set("string", ev.string_value).set("float", ev.float_value)
                    .set("state", state).set("time", ev.time);
                scene->events().emit(owner->name(), "anim_event", args);
            }
        }
    }

    /**
     * @brief Writes a track's sampled channels into `out`, leaving channels with no keys
     *        untouched. `root_ref`, when given, holds this bone's extracted root-motion
     *        channels at their clip-start value (see toy::anim::Animator::hold_root_'s
     *        doc) -- the travel is handed to the owner instead, so the bone itself must not
     *        also carry it, or the rig would travel twice.
     */
    static void write_sampled_(const SkeletalTrack& track, float t, Rig::PoseChannel& out, const BoneRef* root_ref = nullptr) {
        if (!track.location.empty()) {
            float v[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            track.location.sample(t, 3, v);
            if (root_ref && (root_ref->root_translation[0] || root_ref->root_translation[1] || root_ref->root_translation[2])) {
                float start[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                track.location.sample(0.0f, 3, start);
                for (int c = 0; c < 3; ++c) {
                    if (root_ref->root_translation[c]) v[c] = start[c];
                }
            }
            out.location = glm::vec3(v[0], v[1], v[2]);
        }
        if (!track.rotation.empty()) {
            glm::quat q = sample_rotation(track.rotation, t);
            if (root_ref && root_ref->root_yaw) {
                const glm::quat q0 = sample_rotation(track.rotation, 0.0f);
                const auto yaw_of = [](const glm::quat& r) {
                    return glm::degrees(std::atan2(2.0f * (r.w * r.z + r.x * r.y), r.w * r.w + r.x * r.x - r.y * r.y - r.z * r.z));
                };
                const float dyaw = glm::radians(yaw_of(q0) - yaw_of(q));
                q = glm::normalize(glm::angleAxis(dyaw, glm::vec3(0.0f, 0.0f, 1.0f)) * q);
            }
            out.rotation = q;
        }
        if (!track.scale.empty()) {
            float v[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            track.scale.sample(t, 3, v);
            out.scale = glm::vec3(v[0], v[1], v[2]);
        }
    }

    /** @brief Writes one layer's sampled pose onto the Rig outright (full weight, no blend). */
    void apply_layer_only_(const Layer& layer) {
        if (!layer.state || !_rig) return;
        const SkeletalClip* clip = layer.state->resolve();
        if (!clip) return;
        auto it = _state_bindings.find(layer.state);
        if (it == _state_bindings.end()) return;
        const auto& bone_refs = it->second.bone_refs;
        for (size_t i = 0; i < clip->bones.size() && i < bone_refs.size(); ++i) {
            const int bi = bone_refs[i].bone_index;
            if (bi < 0) continue;
            write_sampled_(clip->bones[i], layer.sample_time, _rig->pose(static_cast<size_t>(bi)), &bone_refs[i]);
        }
    }

    /**
     * @brief Advances playback time and crossfade weights, and queues this frame's events. Main
     *        thread only. See toy::anim::Animator::advance_()'s doc.
     */
    void advance_(float dt) {
        if (!_bound) rebind();
        ensure_state_bound_(_current.state);
        if (_crossfading) ensure_state_bound_(_previous.state);
        _pending_events.clear();
        _root_delta = toy::anim::RootMotionDelta{};
        if (_playing) {
            const toy::anim::RootMotionDelta cur = advance_layer_(_current, dt, true);
            toy::anim::RootMotionDelta prev;
            if (_crossfading) {
                prev = advance_layer_(_previous, dt, false); // only the target state fires events
                _crossfade_elapsed += dt;
                const float t = _crossfade_duration > 0.0f ? std::min(1.0f, _crossfade_elapsed / _crossfade_duration) : 1.0f;
                _current.weight = t;
                _previous.weight = 1.0f - t;
                _root_delta.translation = cur.translation * t + prev.translation * (1.0f - t);
                _root_delta.yaw_deg = cur.yaw_deg * t + prev.yaw_deg * (1.0f - t);
                if (t >= 1.0f) {
                    _crossfading = false;
                    _previous = Layer{};
                    _current.weight = 1.0f;
                }
            } else {
                _root_delta = cur;
            }
        }
        dispatch_events_();
    }

    /**
     * @brief Writes this frame's pose onto the sibling Rig, then hands off root motion. Main
     *        thread only.
     *
     * Not crossfading: the current layer's sampled value is written outright, per bone. While
     * crossfading, the PREVIOUS layer is written first as the baseline (every bone it touches,
     * at full weight), then the CURRENT layer is applied on top: a bone touched by both layers
     * slerps/lerps between them by the crossfade weight; a bone touched by only one layer
     * (an unusual, partial clip) simply takes that layer's value outright, treating the other
     * layer as "does not move this bone" rather than snapping once the fade completes.
     */
    void apply_(float /*dt*/) {
        if (!_rig) return;
        if (!_crossfading) {
            apply_layer_only_(_current);
            apply_root_motion_();
            return;
        }

        apply_layer_only_(_previous);

        const SkeletalClip* cur_clip = _current.state ? _current.state->resolve() : nullptr;
        auto cur_it = cur_clip ? _state_bindings.find(_current.state) : _state_bindings.end();
        if (!cur_clip || cur_it == _state_bindings.end()) {
            apply_root_motion_();
            return;
        }

        const SkeletalClip* prev_clip = _previous.state ? _previous.state->resolve() : nullptr;
        auto prev_it = prev_clip ? _state_bindings.find(_previous.state) : _state_bindings.end();
        _touched_scratch.assign(_rig->bone_count(), 0);
        if (prev_clip && prev_it != _state_bindings.end()) {
            const auto& refs = prev_it->second.bone_refs;
            for (size_t i = 0; i < prev_clip->bones.size() && i < refs.size(); ++i) {
                if (refs[i].bone_index >= 0) _touched_scratch[static_cast<size_t>(refs[i].bone_index)] = 1;
            }
        }

        const auto& refs = cur_it->second.bone_refs;
        const float t = std::clamp(_current.weight, 0.0f, 1.0f);
        for (size_t i = 0; i < cur_clip->bones.size() && i < refs.size(); ++i) {
            const int bi = refs[i].bone_index;
            if (bi < 0) continue;
            Rig::PoseChannel& out = _rig->pose(static_cast<size_t>(bi));
            if (_touched_scratch[static_cast<size_t>(bi)]) {
                Rig::PoseChannel cur_sample;
                write_sampled_(cur_clip->bones[i], _current.sample_time, cur_sample, &refs[i]);
                out.location = glm::mix(out.location, cur_sample.location, t);
                out.rotation = glm::slerp(out.rotation, cur_sample.rotation, t);
                out.scale = glm::mix(out.scale, cur_sample.scale, t);
            } else {
                write_sampled_(cur_clip->bones[i], _current.sample_time, out, &refs[i]);
            }
        }
        apply_root_motion_();
    }

    /** @brief Hands this frame's root motion to a receiver on the owner, or moves the owner's Transform. */
    void apply_root_motion_() {
        if (!apply_root_motion || !owner || _root_delta.is_zero()) return;
        for (const auto& comp : owner->components()) {
            if (auto* receiver = dynamic_cast<toy::anim::IRootMotionReceiver*>(comp.get())) {
                if (receiver->consume_root_motion(_root_delta.translation, _root_delta.yaw_deg)) return;
                break;
            }
        }
        auto* tc = owner->get_transform();
        if (!tc) return;
        coopa::util::Transform& t = tc->transform();
        t.set_position(t.position() + t.rotation_quat() * _root_delta.translation);
        if (_root_delta.yaw_deg != 0.0f) {
            glm::vec3 r = t.rotation_degrees();
            r.z += _root_delta.yaw_deg;
            t.set_rotation(r);
        }
    }

    Rig* _rig = nullptr;
    std::vector<std::unique_ptr<SkeletalAnimatorState>>             _states;
    std::unordered_map<SkeletalAnimatorState*, StateBinding>        _state_bindings;

    Layer _current;
    Layer _previous;
    bool  _crossfading = false;
    float _crossfade_elapsed = 0.0f;
    float _crossfade_duration = 0.0f;

    bool  _playing = false;
    bool  _bound = false;
    float _speed = 1.0f;

    std::vector<PendingEvent> _pending_events;
    toy::anim::RootMotionDelta _root_delta;
    std::vector<int8_t> _touched_scratch; ///< Scratch for apply_()'s crossfade path; reused across frames.
};

} // namespace rig
} // namespace toy

#endif // TOYENGINE_RIG_SKELETAL_ANIMATOR_H
