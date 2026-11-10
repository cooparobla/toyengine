/**
 * @file animator.h
 * @brief Component that plays AnimationClips against named states, with crossfade blending.
 */

#ifndef COOPA_ANIMATION_ANIMATOR_H
#define COOPA_ANIMATION_ANIMATOR_H

#include <coopa/animation/animated_property.h>
#include <coopa/animation/animation_clip.h>
#include <coopa/animation/procedural_track.h>
#include <coopa/animation/root_motion.h>
#include <coopa/asset/asset_handle.h>
#include <coopa/event/event_bus.h>
#include <coopa/event/signal.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace coopa {
namespace anim {

class AnimationSystem; // Forward declaration — Animator grants it friend access; see animation_system.h.

/**
 * @struct AnimatorState
 * @brief One named entry on an Animator: a clip (inline or asset-loaded) plus per-state overrides.
 */
struct AnimatorState {
    std::string name;

    /** Set when built programmatically (e.g. the uicoopa demo's inline clips). */
    std::shared_ptr<AnimationClip> inline_clip;
    /** Set when loaded via YAML's `clip:` path — see animation_yaml.h. */
    coopa::asset::AssetHandle<AnimationClip> asset_clip;

    bool     has_wrap_override = false;
    WrapMode wrap_override     = WrapMode::Loop;
    float    speed             = 1.0f; ///< Multiplies the Animator's own speed() while this state plays.

    /** @brief The clip data to sample, or nullptr if not yet loaded/set. */
    const AnimationClip* resolve() const {
        if (inline_clip) return inline_clip.get();
        if (asset_clip.is_loaded()) return asset_clip.get();
        return nullptr;
    }

    /** @brief AssetHandle::revision(), or 0 for an inline clip (which never changes underneath us). */
    uint32_t revision() const { return asset_clip.is_valid() ? asset_clip.revision() : 0; }

    WrapMode effective_wrap(const AnimationClip& clip) const {
        return has_wrap_override ? wrap_override : clip.wrap;
    }
};

/**
 * @class Animator
 * @brief Plays named AnimatorStates, crossfading between them, driving any
 *        number of AnimatedProperty bindings across the scene.
 *
 * Deliberately has NO Component::update()/late_update() override — an
 * Animator never self-drives. It is evaluated only by a registered
 * coopa::anim::AnimationSystem (see animation_system.h), via the private
 * advance_()/evaluate_()/apply_() split (AnimationSystem is a friend). This
 * is what lets AnimationSystem batch work across every Animator in the scene
 * instead of each one ticking independently. If nothing ever animates, check
 * first that an AnimationSystem is actually registered on the scene (see
 * install_animation_system()) — see start()'s doc for why that can't be
 * checked and warned about automatically here.
 *
 * Binding resolution happens once, lazily, and is cached: every track across
 * every state resolves its target object/component/property at bind time,
 * and tracks that resolve to the SAME (Component*, AnimatedProperty*) pair
 * are deduplicated into one shared accumulator — see evaluate_()'s doc for
 * why that dedup is what prevents two tracks (or a crossfading previous/
 * current pair) from clobbering each other.
 */
class Animator : public coopa::scene::Component {
public:
    Animator() { bump_generation_(); }
    ~Animator() override { bump_generation_(); }

    std::string type_name() const override { return "Animator"; }

    /**
     * @brief Process-wide count of Animators created or destroyed so far.
     *
     * AnimationSystem compares it each frame against the value at its last gather, so an
     * Animator spawned at runtime (SceneLoader::spawn(), Engine::spawn(), a scene built while
     * another runs) animates -- and a destroyed one is dropped -- without a manual refresh().
     */
    static uint64_t generation() { return generation_counter_().load(std::memory_order_acquire); }

    /** @brief Name of the state to play automatically once bound. Set before or after start(). */
    std::string auto_play;

    /**
     * @brief Convenience default duration for callers that trigger crossfade()
     *        without picking their own duration each time (e.g. a future
     *        signal-driven "PlayAnimatorOnSignal" reactor in uicoopa). Not
     *        read by Animator itself — crossfade() always takes an explicit
     *        duration.
     */
    float default_crossfade = 0.0f;

    /** @brief Fired exactly once per play()/crossfade() when a WrapMode::Once state reaches its end. */
    coopa::event::Signal<const std::string&> on_state_finished;

    /**
     * @brief Fired for every clip event the current state's playhead crosses (see
     *        AnimationClip::events). Ordered by playback time within a frame; emitted at the end
     *        of the Animation phase's advance step, so a handler may safely play()/crossfade().
     *
     * Each event is also posted to the scene EventBus as (owner name, "anim_event") with args
     * `name`, `string`, `float`, `state` and `time`. While crossfading only the target (current)
     * state fires, so an event never fires twice from the outgoing and incoming clips.
     */
    coopa::event::Signal<const AnimationEvent&> on_event;

    /**
     * @brief Move the owner by the playing clips' root motion (see AnimationClip::root_motion).
     *
     * Off: the delta is still extracted (root_motion_delta()) and the root bone still held in
     * place, but nothing moves the owner -- gameplay code may read and apply it. On: the delta
     * goes to an IRootMotionReceiver on the owner if one accepts it (a CharacterController with
     * use_root_motion, which then collides), else straight onto the owner's Transform.
     */
    bool apply_root_motion = false;

    /**
     * @brief Binds and starts auto_play, if set.
     *
     * Does NOT warn about a missing "Animation" system: SceneLoader::load()
     * (and therefore SceneManager::load_scene()) always calls Scene::start()
     * internally, before the caller has any opportunity to
     * install_animation_system() on the returned Scene — so a check here
     * would false-positive on the ordinary, correct usage pattern every
     * time. If nothing ever animates, the fix is always the same: register
     * coopa::anim::AnimationSystem (see animation_system.h) on the scene.
     */
    void start() override {
        rebind();
        if (!auto_play.empty()) play(auto_play);
    }

    // --- Building states ---

    /** @brief Adds a state backed by a programmatically-built clip (e.g. for tests or an imperative demo). */
    AnimatorState* add_state(const std::string& name, std::shared_ptr<AnimationClip> clip) {
        auto state = std::make_unique<AnimatorState>();
        state->name = name;
        state->inline_clip = std::move(clip);
        AnimatorState* raw = state.get();
        states_.push_back(std::move(state));
        bound_ = false;
        return raw;
    }

    /** @brief Adds a state backed by an asset-loaded clip. */
    AnimatorState* add_state(const std::string& name, coopa::asset::AssetHandle<AnimationClip> clip) {
        auto state = std::make_unique<AnimatorState>();
        state->name = name;
        state->asset_clip = std::move(clip);
        AnimatorState* raw = state.get();
        states_.push_back(std::move(state));
        bound_ = false;
        return raw;
    }

    /** @brief Finds a state by name, or nullptr. */
    AnimatorState* find_state(const std::string& name) const {
        for (const auto& s : states_) {
            if (s->name == name) return s.get();
        }
        return nullptr;
    }

    // --- Playback ---

    /**
     * @brief Immediately switches to `state`, discarding any in-progress crossfade.
     * @param state Name of a state previously added via add_state().
     * @param start_time Initial raw playback time.
     */
    void play(const std::string& state, float start_time = 0.0f) {
        AnimatorState* s = find_state(state);
        if (!s) {
            std::cerr << "[Animator] play(): unknown state '" << state << "'" << std::endl;
            return;
        }
        current_ = Layer{};
        current_.state = s;
        current_.time = start_time;
        current_.weight = 1.0f;
        previous_ = Layer{};
        crossfading_ = false;
        playing_ = true;
        ensure_state_bound_(s);
    }

    /**
     * @brief Crossfades from whatever is currently playing into `state` over `duration` seconds.
     *
     * If nothing is currently playing, behaves like play(). If duration <= 0,
     * behaves like an immediate play() (no blend).
     */
    void crossfade(const std::string& state, float duration) {
        if (!current_.state || duration <= 0.0f) {
            play(state);
            return;
        }
        AnimatorState* s = find_state(state);
        if (!s) {
            std::cerr << "[Animator] crossfade(): unknown state '" << state << "'" << std::endl;
            return;
        }
        previous_ = current_;
        previous_.weight = 1.0f;
        current_ = Layer{};
        current_.state = s;
        current_.time = 0.0f;
        current_.weight = 0.0f;
        crossfading_ = true;
        crossfade_elapsed_ = 0.0f;
        crossfade_duration_ = duration;
        playing_ = true;
        ensure_state_bound_(s);
    }

    /** @brief Freezes playback in place; the last-applied pose remains. */
    void stop() { playing_ = false; }

    /**
     * @brief Stops AND drops the current state, so nothing is applied at all until the next
     *        play()/crossfade(). Unlike stop() -- which keeps re-applying the frozen pose every
     *        frame -- this hands the animated properties to someone else (a ragdoll's physics).
     */
    void clear() {
        current_ = Layer{};
        previous_ = Layer{};
        crossfading_ = false;
        playing_ = false;
    }

    /**
     * @brief Seeks the current state to `time` and applies its pose immediately,
     *        without advancing playback or firing on_state_finished.
     */
    void sample_at(float time) {
        if (!current_.state) return;
        ensure_state_bound_(current_.state);
        const AnimationClip* clip = current_.state->resolve();
        if (!clip) return;
        bool unused_finished = false;
        current_.time = time;
        current_.sample_time = wrap_time_(time, clip->effective_length(), current_.state->effective_wrap(*clip), unused_finished);
        snapshot_procedural_inputs_(current_.state);
        std::vector<float> scratch(bindings_.size() * 8, 0.0f);
        evaluate_layer_(current_, scratch.data());
        apply_(scratch.data());
    }

    void set_speed(float s) { speed_ = s; }
    float speed() const { return speed_; }

    void set_time(float t) { current_.time = t; }
    float time() const { return current_.time; }

    /** @brief Current layer's raw time divided by its clip length, or 0 if no state/clip/length. */
    float normalized_time() const {
        if (!current_.state) return 0.0f;
        const AnimationClip* clip = current_.state->resolve();
        if (!clip) return 0.0f;
        float len = clip->effective_length();
        return len > 0.0f ? current_.time / len : 0.0f;
    }

    bool is_playing() const { return playing_ && current_.state != nullptr; }

    /** @brief This frame's root motion, blended across a crossfade by layer weight (owner-local). */
    const RootMotionDelta& root_motion_delta() const { return root_delta_; }

    const std::string& current_state() const {
        static const std::string empty;
        return current_.state ? current_.state->name : empty;
    }

    /**
     * @brief Re-resolves every track's target object/component/property.
     *
     * Called automatically on start(), on play()/crossfade() into a state
     * whose clip changed since it was last bound, and lazily from advance_()
     * for an Animator whose owning SceneObject was inactive at scene start()
     * (SceneObject::start() skips inactive subtrees, so such an Animator
     * never receives its own start() until reactivated). Safe to call
     * explicitly at any time (e.g. after spawning new targets at runtime).
     */
    void rebind() {
        bindings_.clear();
        for (auto& state : states_) {
            rebind_state_(*state);
        }
        bound_ = true;
    }

    /** @brief Number of deduplicated (Component*, AnimatedProperty*) bindings — for AnimationSystem's scratch sizing. */
    size_t binding_count() const { return bindings_.size(); }

    /** @brief Track count across the currently active layer(s) — what evaluate_() actually samples this frame. */
    size_t active_track_count() const {
        size_t n = 0;
        if (current_.state) {
            if (const AnimationClip* c = current_.state->resolve()) n += c->tracks.size();
        }
        if (crossfading_ && previous_.state) {
            if (const AnimationClip* c = previous_.state->resolve()) n += c->tracks.size();
        }
        return n;
    }

private:
    static std::atomic<uint64_t>& generation_counter_() {
        static std::atomic<uint64_t> counter{0};
        return counter;
    }
    static void bump_generation_() { generation_counter_().fetch_add(1, std::memory_order_acq_rel); }

private:
    friend class AnimationSystem;

    static constexpr size_t kInvalidIndex = static_cast<size_t>(-1);

    /** @brief One resolved (Component*, AnimatedProperty*) accumulation target, shared by every track that writes it. */
    struct Binding {
        coopa::scene::SceneObject* object    = nullptr;
        coopa::scene::Component*   component = nullptr;
        const AnimatedProperty*    prop      = nullptr;
        void*                      target    = nullptr; ///< prop->cast(*component), resolved once.
    };

    /** @brief Per-track resolved state, parallel to one AnimationClip::tracks entry. */
    struct TrackRef {
        size_t                     binding_index = kInvalidIndex;
        uint8_t                    channel_mask  = 0x0F;
        const ProceduralEvaluator* evaluator     = nullptr; ///< Valid only for procedural tracks.
        coopa::scene::SceneObject* proc_target_object = nullptr; ///< Valid only when procedural.target_object is set and resolved.
        ProceduralInputs           inputs; ///< Snapshotted on the main thread each frame in advance_()/sample_at().
        uint8_t                    root_channels = 0;     ///< Position channels extracted as root motion (bit c = channel c).
        bool                       root_yaw      = false; ///< This rotation_quat track's yaw is extracted as root motion.
    };

    /** @brief One playing instance of a state: its own time cursor and crossfade weight. */
    struct Layer {
        AnimatorState* state       = nullptr;
        float          time        = 0.0f; ///< Raw, unwrapped accumulated time.
        float          sample_time = 0.0f; ///< time, wrapped/clamped per the state's WrapMode.
        float          weight      = 1.0f;
        bool           finished_fired = false;
    };

    // Per-state runtime data, keyed by AnimatorState* — kept outside AnimatorState
    // itself so AnimatorState stays a plain "what to play" value the app can
    // construct freely, with only the Animator owning resolution results.
    struct StateBinding {
        std::vector<TrackRef> track_refs;
        uint32_t              bound_revision = 0;
        // Distinguishes "successfully bound against a loaded clip" from
        // "never bound, or last attempt found no clip yet" — both leave
        // bound_revision at its default 0, and an AssetHandle's own
        // revision() is ALSO 0 before its first publish. Without this flag,
        // an Animator whose state was added via add_state() while the clip
        // was still mid-load (the normal case for an asset-backed state
        // parsed straight out of scene YAML) would compare 0 == 0, conclude
        // nothing changed, and never retry once the async load completes.
        bool has_clip = false;
    };

    /**
     * @brief Binds (or re-binds) one state if it has never successfully
     *        bound, or its clip's AssetHandle revision has moved since the
     *        last bind (initial async load completing, or a hot reload).
     *        Cheap to call every frame: one hashmap lookup, one integer
     *        compare, no-op in the common case.
     */
    void ensure_state_bound_(AnimatorState* state) {
        if (!state) return;
        auto it = state_bindings_.find(state);
        if (it == state_bindings_.end() || !it->second.has_clip || it->second.bound_revision != state->revision()) {
            rebind_state_(*state);
        }
    }

    void rebind_state_(AnimatorState& state) {
        const AnimationClip* clip = state.resolve();
        StateBinding& sb = state_bindings_[&state];
        sb.track_refs.clear();
        sb.has_clip = (clip != nullptr);
        if (!clip) return;
        sb.bound_revision = state.revision();
        sb.track_refs.resize(clip->tracks.size());
        for (size_t i = 0; i < clip->tracks.size(); ++i) {
            rebind_track_(*clip, clip->tracks[i], sb.track_refs[i]);
        }
    }

    void rebind_track_(const AnimationClip& clip, const AnimationTrack& track, TrackRef& ref) {
        coopa::scene::SceneObject* target_obj = resolve_object_(track.object_path);
        if (!target_obj) {
            std::cerr << "[Animator] '" << (owner ? owner->name() : std::string("?"))
                      << "': could not resolve target object '" << track.object_path
                      << "' for property '" << track.property << "'" << std::endl;
            return;
        }
        coopa::scene::Component* comp = target_obj->get_component_by_type_name(track.component_type, track.component_index);
        if (!comp) {
            std::cerr << "[Animator] '" << (owner ? owner->name() : std::string("?"))
                      << "': object '" << target_obj->name() << "' has no component '"
                      << track.component_type << "'" << std::endl;
            return;
        }
        uint8_t suffix_mask = 0x0F;
        const AnimatedProperty* prop = AnimatedPropertyRegistry::instance().find(track.component_type, track.property, &suffix_mask);
        if (!prop) {
            std::cerr << "[Animator] '" << (owner ? owner->name() : std::string("?"))
                      << "': no AnimatedProperty registered for " << track.component_type
                      << "." << track.property << std::endl;
            return;
        }
        void* casted = prop->cast(*comp);
        if (!casted) return; // dynamic_cast failed; comp's runtime type doesn't match the property's expectations

        uint8_t valid = static_cast<uint8_t>((1u << prop->component_count) - 1);
        ref.channel_mask = suffix_mask & valid;
        ref.binding_index = find_or_add_binding_(target_obj, comp, prop, casted);

        // Root-motion source: a keyframed Transform position / rotation_quat track on the clip's
        // root_motion.object. Procedural tracks are never extracted.
        if (clip.root_motion.enabled() && track.kind == TrackKind::Keyframed &&
            track.component_type == "Transform" && target_obj == resolve_object_(clip.root_motion.object)) {
            if (prop->name == "position") {
                const uint8_t want = clip.root_motion.translation == RootMotionTranslation::XYZ ? 0x07
                                   : clip.root_motion.translation == RootMotionTranslation::XY  ? 0x03 : 0x00;
                ref.root_channels = ref.channel_mask & want;
            } else if (prop->name == "rotation_quat" && clip.root_motion.yaw && ref.channel_mask == 0x0F) {
                ref.root_yaw = true;
            }
        }

        if (track.kind == TrackKind::Procedural) {
            ref.evaluator = ProceduralTrackRegistry::instance().find(track.procedural.type);
            if (!ref.evaluator) {
                std::cerr << "[Animator] unknown procedural type '" << track.procedural.type << "'" << std::endl;
            }
            if (!track.procedural.target_object.empty()) {
                ref.proc_target_object = resolve_object_(track.procedural.target_object);
            }
        }
    }

    size_t find_or_add_binding_(coopa::scene::SceneObject* obj, coopa::scene::Component* comp,
                                const AnimatedProperty* prop, void* target) {
        for (size_t i = 0; i < bindings_.size(); ++i) {
            if (bindings_[i].component == comp && bindings_[i].prop == prop) return i;
        }
        bindings_.push_back(Binding{obj, comp, prop, target});
        return bindings_.size() - 1;
    }

    /**
     * @brief Resolves a track's object_path against this Animator's owner/scene.
     *
     * Empty -> owner. Contains '/' -> walked segment-by-segment via
     * find_descendant(). Otherwise -> find_descendant() from owner, then
     * scene->find_object() scene-wide.
     */
    coopa::scene::SceneObject* resolve_object_(const std::string& path) const {
        if (!owner) return nullptr;
        if (path.empty()) return owner;
        if (path.find('/') != std::string::npos) {
            coopa::scene::SceneObject* cur = owner;
            size_t start = 0;
            while (cur) {
                size_t slash = path.find('/', start);
                std::string segment = (slash == std::string::npos) ? path.substr(start) : path.substr(start, slash - start);
                if (!segment.empty()) {
                    cur = cur->find_descendant(segment);
                }
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
            return cur;
        }
        if (coopa::scene::SceneObject* found = owner->find_descendant(path)) return found;
        return scene ? scene->find_object(path) : nullptr;
    }

    /**
     * @brief Wraps a raw accumulated time per WrapMode. length <= 0 means
     *        "infinite" (a procedural-only clip with no explicit length) —
     *        time is returned unwrapped and out_just_finished is always false.
     */
    static float wrap_time_(float t, float length, WrapMode wrap, bool& out_just_finished) {
        out_just_finished = false;
        if (length <= 0.0f) return t;
        switch (wrap) {
            case WrapMode::Once:
                if (t >= length) { out_just_finished = true; return length; }
                if (t < 0.0f) return 0.0f;
                return t;
            case WrapMode::PingPong: {
                float period = length * 2.0f;
                float m = std::fmod(t, period);
                if (m < 0.0f) m += period;
                return (m > length) ? (period - m) : m;
            }
            case WrapMode::Loop:
            default: {
                float m = std::fmod(t, length);
                if (m < 0.0f) m += length;
                return m;
            }
        }
    }

    /**
     * @brief Splits raw playback time [t0 -> t1] into the monotonic runs of SAMPLE time it covers,
     *        in playback order, calling f(from, to) for each (from > to while sampling backwards:
     *        negative speed, or the return leg of a ping-pong).
     *
     * Loop: one run per clip cycle crossed. PingPong: odd cycles are mirrored (x -> length - x).
     * Once: one run, clamped to [0, length]. length <= 0 (an infinite clip): one unwrapped run.
     */
    template<typename F>
    static void for_each_sample_run_(float t0, float t1, float length, WrapMode wrap, F&& f) {
        if (t0 == t1) return;
        if (length <= 0.0f) { f(t0, t1); return; }
        if (wrap == WrapMode::Once) {
            const float a = std::clamp(t0, 0.0f, length), b = std::clamp(t1, 0.0f, length);
            if (a != b) f(a, b);
            return;
        }
        const bool forward = t1 > t0;
        const float lo_t = forward ? t0 : t1, hi_t = forward ? t1 : t0;
        long long k_min = static_cast<long long>(std::floor(lo_t / length));
        long long k_max = static_cast<long long>(std::ceil(hi_t / length)) - 1;
        constexpr long long k_max_runs = 1024;   // a runaway dt only ever visits the newest cycles
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
            if (wrap == WrapMode::PingPong && (k & 1)) { from = length - from; to = length - to; }
            f(from, to);
        }
    }

    /**
     * @brief Queues every clip event a sample-time run crosses. Forward runs take [from, to),
     *        backward runs (to, from] -- so each endpoint of a ping-pong fires once per visit and
     *        an event exactly at the clip's end fires when a forward Loop/Once run reaches it.
     */
    void collect_events_(const AnimationClip& clip, WrapMode wrap, float from, float to, const std::string& state) {
        const float len = clip.effective_length();
        const size_t first = pending_events_.size();
        for (const AnimationEvent& ev : clip.events) {
            bool hit;
            if (to > from) hit = (ev.time >= from && ev.time < to) ||
                                 (wrap != WrapMode::PingPong && len > 0.0f && to >= len && ev.time >= len && from < len);
            else hit = ev.time > to && ev.time <= from;
            if (hit) pending_events_.push_back(PendingEvent{&ev, &state});
        }
        // Events are sorted ascending; a backward run visits them latest-first.
        if (to < from) std::reverse(pending_events_.begin() + static_cast<std::ptrdiff_t>(first), pending_events_.end());
    }

    /** @brief The root bone's extracted channels at sample time `s`: position (unextracted channels 0) and yaw (degrees). */
    void sample_root_(const AnimationClip& clip, const StateBinding& sb, float s, glm::vec3& pos, float& yaw_deg) const {
        pos = glm::vec3(0.0f);
        yaw_deg = 0.0f;
        for (size_t i = 0; i < clip.tracks.size() && i < sb.track_refs.size(); ++i) {
            const TrackRef& ref = sb.track_refs[i];
            if (!ref.root_channels && !ref.root_yaw) continue;
            const uint8_t active_count = popcount4_(ref.channel_mask);
            float tmp[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            clip.tracks[i].curve.sample(s, active_count, tmp);
            if (ref.root_yaw) {
                yaw_deg = yaw_of_(tmp);
                continue;
            }
            uint8_t k = 0;
            for (uint8_t c = 0; c < 3; ++c) {
                if (!(ref.channel_mask & (1u << c))) continue;
                if (ref.root_channels & (1u << c)) pos[c] = tmp[k];
                ++k;
            }
        }
    }

    /** @brief Yaw (degrees about +Z) of an xyzw quaternion; scale-invariant, since a keyframed
     *         quaternion track interpolates componentwise and isn't unit between keys. */
    static float yaw_of_(const float* q) {
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        return glm::degrees(std::atan2(2.0f * (w * z + x * y), w * w + x * x - y * y - z * z));
    }

    /** @brief The root motion a sample-time run moves through, expressed in the frame the run started in. */
    RootMotionDelta root_run_delta_(const AnimationClip& clip, const StateBinding& sb, float from, float to) const {
        glm::vec3 p0, p1;
        float y0, y1;
        sample_root_(clip, sb, from, p0, y0);
        sample_root_(clip, sb, to, p1, y1);
        RootMotionDelta d;
        d.yaw_deg = std::remainder(y1 - y0, 360.0f);
        d.translation = p1 - p0;
        if (y0 != 0.0f) {   // translate in the facing the run started with, not the clip's frame
            const float r = glm::radians(-y0), c = std::cos(r), sn = std::sin(r);
            d.translation = glm::vec3(c * d.translation.x - sn * d.translation.y,
                                      sn * d.translation.x + c * d.translation.y, d.translation.z);
        }
        return d;
    }

    /**
     * @brief Advances one layer's clock; queues its crossed events (when `fire_events`) and
     *        returns the root motion it moved through this frame.
     */
    RootMotionDelta advance_layer_(Layer& layer, float dt, bool fire_events) {
        RootMotionDelta delta;
        if (!layer.state) return delta;
        const AnimationClip* clip = layer.state->resolve();
        if (!clip) return delta;
        const float t0 = layer.time;
        layer.time += dt * speed_ * layer.state->speed;
        const float length = clip->effective_length();
        const WrapMode wrap = layer.state->effective_wrap(*clip);
        bool just_finished = false;
        layer.sample_time = wrap_time_(layer.time, length, wrap, just_finished);

        const bool want_events = fire_events && !clip->events.empty();
        const StateBinding* sb = nullptr;
        if (clip->root_motion.enabled()) {
            auto it = state_bindings_.find(layer.state);
            if (it != state_bindings_.end()) sb = &it->second;
        }
        if (want_events || sb) {
            for_each_sample_run_(t0, layer.time, length, wrap, [&](float from, float to) {
                if (want_events) collect_events_(*clip, wrap, from, to, layer.state->name);
                if (sb) {
                    const RootMotionDelta d = root_run_delta_(*clip, *sb, from, to);
                    // Successive runs chain: rotate each into the facing accumulated so far.
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

    void snapshot_procedural_inputs_(AnimatorState* state) {
        if (!state) return;
        const AnimationClip* clip = state->resolve();
        if (!clip) return;
        auto it = state_bindings_.find(state);
        if (it == state_bindings_.end()) return;
        auto& track_refs = it->second.track_refs;
        for (size_t i = 0; i < clip->tracks.size() && i < track_refs.size(); ++i) {
            if (clip->tracks[i].kind != TrackKind::Procedural) continue;
            TrackRef& ref = track_refs[i];
            if (!ref.proc_target_object) {
                ref.inputs.target_valid = false;
                continue;
            }
            auto* tc = ref.proc_target_object->get_transform();
            if (tc) {
                ref.inputs.target_position = tc->transform().position();
                ref.inputs.target_valid = true;
            } else {
                ref.inputs.target_valid = false;
            }
        }
    }

    /**
     * @brief Advances playback time and crossfade weights. Main thread only.
     *
     * Lazily rebinds if this Animator never received start() (an Animator on
     * a SceneObject that was inactive at Scene::start() time never gets its
     * own start() called — see SceneObject::start()'s early-return on
     * inactive objects). Also re-checks the currently active layer(s)' own
     * binding every frame via ensure_state_bound_() — cheap when nothing
     * changed, and what lets a state whose clip was still mid-load at bind
     * time pick up its tracks the moment the async load completes. Snapshots
     * every active procedural track's target_object position here, on the
     * main thread, so evaluate_() never touches the scene.
     */
    void advance_(float dt) {
        if (!bound_) rebind();
        ensure_state_bound_(current_.state);
        if (crossfading_) ensure_state_bound_(previous_.state);
        pending_events_.clear();
        root_delta_ = RootMotionDelta{};
        if (playing_) {
            const RootMotionDelta cur = advance_layer_(current_, dt, true);
            RootMotionDelta prev;
            if (crossfading_) {
                prev = advance_layer_(previous_, dt, false);   // only the target state fires events
                crossfade_elapsed_ += dt;
                float t = crossfade_duration_ > 0.0f ? std::min(1.0f, crossfade_elapsed_ / crossfade_duration_) : 1.0f;
                current_.weight = t;
                previous_.weight = 1.0f - t;
                root_delta_.translation = cur.translation * t + prev.translation * (1.0f - t);
                root_delta_.yaw_deg = cur.yaw_deg * t + prev.yaw_deg * (1.0f - t);
                if (t >= 1.0f) {
                    crossfading_ = false;
                    previous_ = Layer{};
                    current_.weight = 1.0f;
                }
            } else {
                root_delta_ = cur;
            }
        }
        snapshot_procedural_inputs_(current_.state);
        if (crossfading_) snapshot_procedural_inputs_(previous_.state);
        dispatch_events_();
    }

    /** @brief Emits the events advance_() queued, after all playback state is settled. */
    void dispatch_events_() {
        if (pending_events_.empty()) return;
        // Copy out first: a handler may play()/crossfade() (or even advance) this Animator.
        std::vector<PendingEvent> events;
        events.swap(pending_events_);
        for (const PendingEvent& pe : events) {
            const AnimationEvent ev = *pe.event;   // the clip may hot-reload under a handler
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
     * @brief Samples every active layer's tracks into a caller-owned scratch buffer.
     *
     * WORKER-SAFE: reads only const clip data and the ProceduralInputs
     * snapshot advance_() already wrote; touches no coopa::scene::Component,
     * no SceneObject, no Scene. `scratch` must have room for
     * binding_count()*8 floats (4 weighted-value accumulators + 4 weight
     * accumulators per binding) and is zeroed here before accumulating.
     *
     * Every track resolving to the same (Component*, AnimatedProperty*) pair
     * shares one binding's accumulator slot — see rebind_track_()'s dedup.
     * That is what lets two tracks on, say, position.x and position.z (or a
     * crossfading previous/current pair on the same property) contribute
     * without clobbering each other: apply_() renormalizes by accumulated
     * weight rather than each track independently calling set().
     */
    void evaluate_(float* scratch) const {
        std::fill(scratch, scratch + bindings_.size() * 8, 0.0f);
        evaluate_layer_(current_, scratch);
        if (crossfading_) evaluate_layer_(previous_, scratch);
    }

    /** @brief Counts the set bits in a 4-bit channel mask. */
    static uint8_t popcount4_(uint8_t mask) {
        uint8_t n = 0;
        for (uint8_t c = 0; c < 4; ++c) {
            if (mask & (1u << c)) ++n;
        }
        return n;
    }

    void evaluate_layer_(const Layer& layer, float* scratch) const {
        if (!layer.state || layer.weight <= 0.0f) return;
        const AnimationClip* clip = layer.state->resolve();
        if (!clip) return;
        auto it = state_bindings_.find(layer.state);
        if (it == state_bindings_.end()) return;
        const auto& track_refs = it->second.track_refs;
        for (size_t i = 0; i < clip->tracks.size() && i < track_refs.size(); ++i) {
            const TrackRef& ref = track_refs[i];
            if (ref.binding_index == kInvalidIndex) continue;
            const AnimationTrack& track = clip->tracks[i];

            // A track with a narrowing channel suffix (e.g. "position.y") is
            // authored as a dense curve/evaluator over just its active
            // channels — a bare scalar for one channel, a {x,y} pair for
            // two, etc. — not a full-width vec3 with only one slot used. So
            // sample exactly popcount(channel_mask) values, densely packed
            // at tmp[0..active_count-1], then scatter them onto the
            // property's real channel indices in ascending bit order.
            uint8_t active_count = popcount4_(ref.channel_mask);
            if (active_count == 0) continue;

            float tmp[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (track.kind == TrackKind::Keyframed) {
                track.curve.sample(layer.sample_time, active_count, tmp);
                if (ref.root_channels || ref.root_yaw) hold_root_(track, ref, active_count, tmp);
            } else if (ref.evaluator) {
                (*ref.evaluator)(track.procedural.params, ref.inputs, layer.sample_time, active_count, tmp);
            } else {
                continue;
            }

            float* value  = scratch + ref.binding_index * 8;
            float* weight = value + 4;
            uint8_t k = 0;
            for (uint8_t c = 0; c < 4; ++c) {
                if (!(ref.channel_mask & (1u << c))) continue;
                value[c]  += tmp[k] * layer.weight;
                weight[c] += layer.weight;
                ++k;
            }
        }
    }

    /**
     * @brief Holds a root-motion track's extracted channels at their clip-start value (the travel
     *        is handed to the owner instead). Worker-safe: const clip data only.
     */
    static void hold_root_(const AnimationTrack& track, const TrackRef& ref, uint8_t active_count, float* tmp) {
        float start[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        track.curve.sample(0.0f, active_count, start);
        if (ref.root_yaw) {
            const float dyaw = glm::radians(yaw_of_(start) - yaw_of_(tmp));
            const glm::quat q = glm::angleAxis(dyaw, glm::vec3(0.0f, 0.0f, 1.0f)) * glm::quat(tmp[3], tmp[0], tmp[1], tmp[2]);
            tmp[0] = q.x; tmp[1] = q.y; tmp[2] = q.z; tmp[3] = q.w;
            return;
        }
        uint8_t k = 0;
        for (uint8_t c = 0; c < 4; ++c) {
            if (!(ref.channel_mask & (1u << c))) continue;
            if (ref.root_channels & (1u << c)) tmp[k] = start[k];
            ++k;
        }
    }

    /** @brief Hands this frame's root motion to a receiver on the owner, or moves the owner's Transform. Main thread. */
    void apply_root_motion_() {
        if (!apply_root_motion || !owner || root_delta_.is_zero()) return;
        for (const auto& comp : owner->components()) {
            if (auto* receiver = dynamic_cast<IRootMotionReceiver*>(comp.get())) {
                if (receiver->consume_root_motion(root_delta_.translation, root_delta_.yaw_deg)) return;
                break;
            }
        }
        auto* tc = owner->get_transform();
        if (!tc) return;
        coopa::util::Transform& t = tc->transform();
        t.set_position(t.position() + t.rotation_quat() * root_delta_.translation);
        if (root_delta_.yaw_deg != 0.0f) {
            // Euler ZYX: Z is outermost, so adding to it turns about the parent's +Z.
            glm::vec3 r = t.rotation_degrees();
            r.z += root_delta_.yaw_deg;
            t.set_rotation(r);
        }
    }

    /**
     * @brief Writes the accumulated scratch buffer back onto every binding's target. Main thread only.
     *
     * A channel is only written if it received nonzero accumulated weight —
     * this is a genuine read-modify-write (via prop->get() first) so a
     * binding with, say, only its Y channel driven this frame leaves X and Z
     * exactly as some other system (or this Animator's own other tracks)
     * last set them.
     */
    void apply_(const float* scratch) {
        for (size_t i = 0; i < bindings_.size(); ++i) {
            const Binding& b = bindings_[i];
            const float* value  = scratch + i * 8;
            const float* weight = value + 4;
            float cur[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            b.prop->get(b.target, cur);
            bool changed = false;
            for (uint8_t c = 0; c < b.prop->component_count; ++c) {
                if (weight[c] > 1e-6f) {
                    cur[c] = value[c] / weight[c];
                    changed = true;
                }
            }
            if (changed) b.prop->set(b.target, cur);
        }
    }

    std::vector<std::unique_ptr<AnimatorState>>                    states_;
    std::unordered_map<AnimatorState*, StateBinding>                state_bindings_;
    std::vector<Binding>                                            bindings_;

    Layer current_;
    Layer previous_;
    bool  crossfading_        = false;
    float crossfade_elapsed_  = 0.0f;
    float crossfade_duration_ = 0.0f;

    bool  playing_ = false;
    bool  bound_   = false;
    float speed_   = 1.0f;

    /** @brief An event crossed this frame, emitted by dispatch_events_() once advance_() is done. */
    struct PendingEvent {
        const AnimationEvent* event = nullptr;
        const std::string*    state = nullptr;
    };
    std::vector<PendingEvent> pending_events_;
    RootMotionDelta           root_delta_;
};

} // namespace anim
} // namespace coopa

#endif // COOPA_ANIMATION_ANIMATOR_H
