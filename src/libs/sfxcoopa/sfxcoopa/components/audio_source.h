/**
 * @file audio_source.h
 * @brief AudioSource: a scene component that plays one clip through
 * SfxResources' AudioEngine, optionally spatialized at its owner's world
 * position.
 *
 * Gain/pitch actually played = component volume/pitch multiplied by the
 * clip's `.import` sidecar volume/pitch (AudioEngine::play() itself ignores
 * those sidecar fields; this component applies them). When `spatialize` is
 * set, the component's `spatial` params replace the clip's sidecar spatial
 * defaults, with position/velocity driven from the owner each update(dt).
 *
 * Never throws for content problems: a missing engine, an unloadable clip,
 * an unknown bus (falls back to Master) or a stereo clip asked to spatialize
 * (falls back to 2D) each log one warning per component and carry on.
 */

#ifndef SFXCOOPA_COMPONENTS_AUDIO_SOURCE_H
#define SFXCOOPA_COMPONENTS_AUDIO_SOURCE_H

#include <sfxcoopa/components/sfx_resources.h>
#include <sfxcoopa/core/engine.h>
#include <sfxcoopa/spatial/panner.h>

#include <coopa/debug/logger.h>
#include <coopa/event/signal.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene_object.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <string>

namespace coopa {
namespace sfx {
namespace components {

/**
 * @class AudioSource
 * @brief Plays `clip_path` on `bus`; plays on the first update() when play_on_start is set.
 *
 * on_finished fires (from update()) when a playing, non-looping voice ends on
 * its own -- not on an explicit stop(). Destroying the component (or its
 * SceneObject) stops its voice.
 */
class AudioSource : public coopa::scene::Component {
public:
    std::string clip_path;      /**< As written; resolved through SfxResources::resolve() at play(). */
    std::string bus = "SFX";    /**< Bus name; falls back to Master if the engine has no such bus. */
    float volume = 1.0f;        /**< Linear gain, multiplied with the clip sidecar's volume. */
    float pitch = 1.0f;         /**< Playback-rate multiplier, multiplied with the clip sidecar's pitch. */
    bool loop = false;
    bool play_on_start = false;
    bool spatialize = false;    /**< Binaural 3D at the owner's world position (clip must be mono). */
    int priority = 0;           /**< Voice-steal priority. */
    coopa::sfx::spatial::SpatialParams spatial{}; /**< Attenuation/cone/doppler; position+velocity set from owner. */

    coopa::event::Signal<> on_finished;

    AudioSource() = default;
    AudioSource(const AudioSource&) = delete;
    AudioSource& operator=(const AudioSource&) = delete;
    ~AudioSource() override { stop(); }

    std::string type_name() const override { return "AudioSource"; }

    /**
     * play_on_start plays on the first update(), not in start(): SceneLoader starts a scene as
     * it loads it, before a host can mark it non-simulating -- an editor loading a scene to
     * edit it must stay silent, and update() only runs while the scene simulates.
     */
    void start() override {
        if (play_on_start) pending_start_ = true;
    }

    void update(float delta_time) override {
        if (pending_start_) {
            pending_start_ = false;
            play();
        }
        coopa::sfx::core::AudioEngine* engine = live_engine_();
        if (!engine || !handle_.is_valid()) return;

        if (!engine->is_voice_active(handle_)) {
            handle_ = {};
            paused_ = false;
            on_finished.emit();
            return;
        }

        if (spatialized_) {
            const glm::vec3 pos = world_position_();
            glm::vec3 velocity(0.0f);
            if (delta_time > 1e-6f) velocity = (pos - prev_pos_) / delta_time;
            prev_pos_ = pos;
            engine->set_voice_motion(handle_, pos, velocity);
        }
    }

    /**
     * @brief (Re)starts playback from the beginning. Stops any voice this
     * component already owns first.
     * @return True if a voice was started.
     */
    bool play() {
        stop();
        coopa::sfx::core::AudioEngine* engine = SfxResources::instance().engine();
        if (!engine) {
            warn_once_(k_warn_no_engine, "no AudioEngine set (SfxResources::set_engine); '" + clip_path + "' not played");
            return false;
        }
        if (clip_path.empty()) {
            warn_once_(k_warn_load, "empty clip path");
            return false;
        }

        const std::string path = SfxResources::instance().resolve(clip_path);
        std::shared_ptr<coopa::sfx::data::AudioClip> clip;
        try {
            clip = engine->load_clip(path);
        } catch (const std::exception& e) {
            warn_once_(k_warn_load, "failed to load clip '" + path + "': " + e.what());
            return false;
        }
        const coopa::sfx::data::ClipImportSettings& sidecar = clip->settings();

        coopa::sfx::core::PlayParams params;
        if (engine->find_bus(bus)) {
            params.bus_name = bus;
        } else {
            if (!bus.empty()) warn_once_(k_warn_bus, "bus '" + bus + "' not found; using Master");
            params.bus_name = engine->master().name();
        }
        params.gain = volume * sidecar.volume;
        params.pitch = pitch * sidecar.pitch;
        params.loop = loop;
        params.priority = priority;

        bool do_3d = spatialize;
        if (do_3d && clip->format().channels > 1) {
            warn_once_(k_warn_stereo, "clip '" + path + "' is stereo; spatialize needs force_mono in its .import -- playing 2D");
            do_3d = false;
        }

        try {
            if (do_3d) {
                const glm::vec3 pos = world_position_();
                coopa::sfx::spatial::SpatialParams sp = spatial;
                sp.position = pos;
                sp.velocity = glm::vec3(0.0f);
                params.spatial = sp;
                handle_ = engine->play_3d(path, pos, params);
                prev_pos_ = pos;
            } else {
                handle_ = engine->play(path, params);
            }
        } catch (const std::exception& e) {
            warn_once_(k_warn_load, "failed to play '" + path + "': " + e.what());
            handle_ = {};
            return false;
        }
        engine_ = engine;
        spatialized_ = do_3d;
        paused_ = false;
        return handle_.is_valid();
    }

    /** @brief Stops (with the engine's short fade) the voice this component owns, if any. No on_finished. */
    void stop() {
        if (handle_.is_valid()) {
            if (coopa::sfx::core::AudioEngine* engine = live_engine_()) engine->stop(handle_);
        }
        handle_ = {};
        paused_ = false;
        spatialized_ = false;
    }

    void pause() {
        coopa::sfx::core::AudioEngine* engine = live_engine_();
        if (engine && handle_.is_valid() && !paused_) {
            engine->pause(handle_);
            paused_ = true;
        }
    }

    void resume() {
        coopa::sfx::core::AudioEngine* engine = live_engine_();
        if (engine && handle_.is_valid() && paused_) {
            engine->resume(handle_);
            paused_ = false;
        }
    }

    /** @brief True while this component owns a live, unpaused voice. */
    bool is_playing() const {
        coopa::sfx::core::AudioEngine* engine = live_engine_();
        return engine && !paused_ && engine->is_voice_active(handle_);
    }

    bool is_paused() const { return paused_ && handle_.is_valid(); }

    /** @brief The engine voice this component currently owns (invalid when idle). */
    coopa::sfx::core::VoiceHandle voice() const { return handle_; }

private:
    bool pending_start_ = false;
    enum WarnBit : uint32_t {
        k_warn_no_engine = 1u << 0,
        k_warn_load = 1u << 1,
        k_warn_bus = 1u << 2,
        k_warn_stereo = 1u << 3,
    };

    /** @brief The engine our voice lives in, only if it is still the registered one (not destroyed/swapped). */
    coopa::sfx::core::AudioEngine* live_engine_() const {
        coopa::sfx::core::AudioEngine* current = SfxResources::instance().engine();
        return (engine_ && engine_ == current) ? engine_ : nullptr;
    }

    glm::vec3 world_position_() const {
        if (owner) {
            if (auto* tc = owner->get_transform()) return glm::vec3(tc->get_world_matrix()[3]);
        }
        return glm::vec3(0.0f);
    }

    void warn_once_(WarnBit bit, const std::string& message) {
        if (warned_ & bit) return;
        warned_ |= bit;
        static coopa::debug::Logger logger("AudioSource");
        const std::string who = owner ? (" on '" + owner->name() + "'") : std::string();
        logger.warn("AudioSource" + who + ": " + message);
    }

    coopa::sfx::core::AudioEngine* engine_ = nullptr;
    coopa::sfx::core::VoiceHandle handle_{};
    glm::vec3 prev_pos_{0.0f};
    bool spatialized_ = false;
    bool paused_ = false;
    uint32_t warned_ = 0;
};

}  // namespace components
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_COMPONENTS_AUDIO_SOURCE_H
