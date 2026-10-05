/**
 * @file audio_system.h
 * @brief The engine's audio: one sfxcoopa AudioEngine, its buses, and the sound card.
 *
 *   Master
 *    ├─ Music
 *    ├─ SFX     (AudioSource's default bus, play_oneshot())
 *    └─ UI      (uicoopa's UiAudio: button clicks and other named UI sounds)
 *
 * Owned by toy::core::Engine, which registers it with sfxcoopa's scene bridge (SfxResources) so
 * AudioSource / AudioListener components in scene YAML play through it, and with UiAudio so the
 * UI sound components share the same engine and device. Never fatal: when no sound card opens
 * (or a headless run asks for none) it falls back to miniaudio's null backend, and everything
 * plays silently.
 *
 * Main-thread API; the device callback renders on miniaudio's own thread.
 */

#ifndef TOYENGINE_AUDIO_AUDIO_SYSTEM_H
#define TOYENGINE_AUDIO_AUDIO_SYSTEM_H

#include <sfxcoopa/components/audio_listener.h>
#include <sfxcoopa/components/sfx_resources.h>
#include <sfxcoopa/core/device.h>
#include <sfxcoopa/core/engine.h>

#include <uicoopa/audio/ui_audio.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace toy::audio {

/** @brief How the AudioSystem opens its output. */
struct AudioSystemOptions {
    uint32_t sample_rate = 48000;
    /// False: no device at all (tests drive render_offline() themselves).
    bool open_device = true;
    /// Open miniaudio's null backend (a timer thread, no sound card): headless runs.
    bool null_backend = false;
};

/** @brief Names of the standard buses. */
inline constexpr const char* k_bus_master = "Master";
inline constexpr const char* k_bus_music = "Music";
inline constexpr const char* k_bus_sfx = "SFX";
inline constexpr const char* k_bus_ui = "UI";

class AudioSystem {
public:
    explicit AudioSystem(const AudioSystemOptions& opt = {})
        : format_{opt.sample_rate, 2}, engine_(format_) {
        music_ = &engine_.create_bus(k_bus_music, engine_.master());
        sfx_ = &engine_.create_bus(k_bus_sfx, engine_.master());
        ui_bus_ = &engine_.create_bus(k_bus_ui, engine_.master());
#ifdef UICOOPA_HAS_AUDIO
        ui_ = std::make_unique<coopa::ui::UiAudio>(engine_, *ui_bus_);
#endif
        if (!opt.open_device) return;
        coopa::sfx::core::DeviceConfig dc = coopa::sfx::core::DeviceConfig::from_env();
        dc.sample_rate = opt.sample_rate;
        if (opt.null_backend) dc.null_backend = true;
        auto callback = [this](float* out, uint32_t frames) {
            if (paused_.load(std::memory_order_relaxed)) {
                std::memset(out, 0, sizeof(float) * frames * format_.channels);
                return;
            }
            engine_.render_offline(out, frames);
        };
        try {
            device_ = std::make_unique<coopa::sfx::core::AudioDevice>(dc, callback);
            null_device_ = dc.null_backend;
        } catch (const std::exception& e) {
            std::cerr << "[toyengine] Audio: no output device (" << e.what() << "); playing silently\n";
            if (!dc.null_backend) {
                dc.null_backend = true;
                try {
                    device_ = std::make_unique<coopa::sfx::core::AudioDevice>(dc, callback);
                    null_device_ = true;
                } catch (const std::exception&) {}
            }
        }
    }

    ~AudioSystem() {
        // Stop the device thread before the engine it renders goes away.
        device_.reset();
    }

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    coopa::sfx::core::AudioEngine& engine() { return engine_; }

    /** @brief The running Engine's audio (set by toy::core::Engine), for components like VolumeBinding. */
    static AudioSystem* active() { return active_ptr_(); }
    static void set_active(AudioSystem* a) { active_ptr_() = a; }
#ifdef UICOOPA_HAS_AUDIO
    coopa::ui::UiAudio* ui() { return ui_.get(); }
#endif
    /** @brief True when a real (non-null) sound card is open. */
    bool has_output() const { return device_ && !null_device_; }

    /** @brief A bus by name (Master, Music, SFX, UI, or any created later). */
    coopa::sfx::mixer::MixerBus* bus(const std::string& name) {
        if (name == k_bus_master) return &engine_.master();
        return engine_.find_bus(name);
    }

    /** @brief Linear volume (0..1+) of a bus; false if no such bus. */
    bool set_bus_volume(const std::string& name, float linear) {
        coopa::sfx::mixer::MixerBus* b = bus(name);
        if (!b) return false;
        b->set_volume(std::max(0.0f, linear));
        return true;
    }
    float bus_volume(const std::string& name) {
        coopa::sfx::mixer::MixerBus* b = bus(name);
        return b ? b->volume() : 0.0f;
    }

    /**
     * @brief Fire-and-forget 2D sound (a pickup, a hit): `path` is resolved against the asset
     *        roots. Returns an invalid handle (and logs once) if the clip can't load.
     */
    coopa::sfx::mixer::VoiceHandle play_oneshot(const std::string& path, const std::string& bus_name = k_bus_sfx,
                                                float gain = 1.0f, float pitch = 1.0f) {
        coopa::sfx::core::PlayParams p;
        p.bus_name = bus_name;
        p.gain = gain;
        p.pitch = pitch;
        try {
            return track_(engine_.play(coopa::sfx::SfxResources::instance().resolve(path), p));
        } catch (const std::exception& e) {
            warn_(path, e.what());
            return {};
        }
    }

    /** @brief play_oneshot() at a world position (mono clips; stereo plays 2D). */
    coopa::sfx::mixer::VoiceHandle play_oneshot_3d(const std::string& path, const glm::vec3& position,
                                                   const std::string& bus_name = k_bus_sfx, float gain = 1.0f) {
        coopa::sfx::core::PlayParams p;
        p.bus_name = bus_name;
        p.gain = gain;
        const std::string resolved = coopa::sfx::SfxResources::instance().resolve(path);
        try {
            return track_(engine_.play_3d(resolved, position, p));
        } catch (const std::exception&) {
            return play_oneshot(path, bus_name, gain);
        }
    }

    /** @brief Freezes all audio output (game paused); resume_all() continues where it stopped. */
    void pause_all() { paused_ = true; }
    void resume_all() { paused_ = false; }
    bool paused() const { return paused_; }

    /** @brief Stops every one-shot this system started (AudioSources stop with their objects). */
    void stop_all() {
        for (const auto& h : oneshots_) engine_.stop(h);
        oneshots_.clear();
    }

    /**
     * @brief Per frame, after the scene updated: the listener follows an AudioListener if one
     *        pushed this frame, else `camera_world` (when given); then the engine's main-thread
     *        bookkeeping (finished voices, clip GC, signals).
     */
    void update(float dt, const glm::mat4* camera_world) {
        auto& res = coopa::sfx::SfxResources::instance();
        if (!res.consume_listener_pushed() && camera_world) {
            coopa::sfx::components::apply_listener_matrix(engine_, *camera_world);
        }
        engine_.update(dt);
        oneshots_.erase(std::remove_if(oneshots_.begin(), oneshots_.end(),
                                       [&](const coopa::sfx::mixer::VoiceHandle& h) { return !engine_.is_voice_active(h); }),
                        oneshots_.end());
    }

private:
    static AudioSystem*& active_ptr_() {
        static AudioSystem* a = nullptr;
        return a;
    }
    coopa::sfx::mixer::VoiceHandle track_(coopa::sfx::mixer::VoiceHandle h) {
        oneshots_.push_back(h);
        return h;
    }
    void warn_(const std::string& path, const std::string& why) {
        if (std::find(warned_.begin(), warned_.end(), path) != warned_.end()) return;
        warned_.push_back(path);
        std::cerr << "[toyengine] Audio: cannot play '" << path << "': " << why << "\n";
    }

    coopa::sfx::data::AudioFormat format_;
    coopa::sfx::core::AudioEngine engine_;
    coopa::sfx::mixer::MixerBus* music_ = nullptr;
    coopa::sfx::mixer::MixerBus* sfx_ = nullptr;
    coopa::sfx::mixer::MixerBus* ui_bus_ = nullptr;
#ifdef UICOOPA_HAS_AUDIO
    std::unique_ptr<coopa::ui::UiAudio> ui_;
#endif
    std::unique_ptr<coopa::sfx::core::AudioDevice> device_;   // last: destroyed first (see ~AudioSystem)
    bool null_device_ = false;
    std::atomic<bool> paused_{false};
    std::vector<coopa::sfx::mixer::VoiceHandle> oneshots_;
    std::vector<std::string> warned_;
};

} // namespace toy::audio

#endif // TOYENGINE_AUDIO_AUDIO_SYSTEM_H
