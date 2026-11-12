/**
 * @file ui_audio.h
 * @brief UiAudio: the one object an application constructs to play named UI
 * sound effects through sfxcoopa.
 *
 * Only compiled when UICOOPA_HAS_AUDIO is defined (see uicoopa/CMakeLists.txt's
 * UICOOPA_WITH_AUDIO option) -- this header is a no-op otherwise, so any
 * translation unit that includes it unconditionally still builds with audio
 * disabled, it just gets nothing. Call sites that actually *use* UiAudio must
 * still guard themselves with `#ifdef UICOOPA_HAS_AUDIO` (see
 * test_settings_builder.cpp), since the type itself doesn't exist otherwise.
 */

#ifndef UICOOPA_AUDIO_UI_AUDIO_H
#define UICOOPA_AUDIO_UI_AUDIO_H

#ifdef UICOOPA_HAS_AUDIO

#include <sfxcoopa/core/engine.h>
#include <sfxcoopa/core/device.h>

#include <uicoopa/audio/sound_library.h>

#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_set>

namespace coopa {
namespace ui {

/** @brief Construction-time tuning for UiAudio. */
struct UiAudioConfig {
    uint32_t sample_rate = 48000;
    bool open_device = true;   /**< False = engine only; caller manually drives update()/render_offline()
                                     (tests, offline bounce, CI with no null-backend thread at all). */
    bool null_backend = false; /**< Forces miniaudio's null backend even if SFX_DEVICE isn't "null" --
                                     still spins a device thread on a timer, unlike open_device=false. */
};

/**
 * @class UiAudio
 * @brief Owns a coopa::sfx::core::AudioEngine, a "UI" bus under Master, and
 * (unless configured otherwise) a real coopa::sfx::core::AudioDevice pumping it --
 * or, via the borrowing constructor, plays through a host-owned engine and bus.
 *
 * Every SoundLibrary lookup and play() call happens by name, resolved
 * through SoundLibrary::instance() -- callers never touch a file path.
 * Never fatal: if the device genuinely fails to open (no sound card, and
 * neither null_backend nor SFX_DEVICE=null was requested), the constructor
 * catches the exception, available() reports false, and every play() becomes
 * a silent no-op rather than piling up voices nothing will ever render.
 *
 * @code
 * UiAudio audio;                     // opens the device (or null backend) in the ctor
 * UiAudio::set_active(&audio);       // process-wide, mirrors FontDefaults::font
 * audio.play("ui_click_soft");
 * // once per frame:
 * audio.update(dt);
 * @endcode
 */
class UiAudio {
public:
    explicit UiAudio(const UiAudioConfig& config = {}) : format_{config.sample_rate, 2} {
        owned_engine_.emplace(format_);
        engine_ = &*owned_engine_;
        ui_bus_ = &engine_->create_bus("UI", engine_->master());
        if (!config.open_device) return; // manual/headless mode -- not a failure, see UiAudioConfig::open_device.

        coopa::sfx::core::DeviceConfig device_config = coopa::sfx::core::DeviceConfig::from_env();
        device_config.sample_rate = config.sample_rate;
        if (config.null_backend) device_config.null_backend = true;

        try {
            device_ = std::make_unique<coopa::sfx::core::AudioDevice>(
                device_config, [this](float* out, uint32_t frames) { engine_->render_offline(out, frames); });
        } catch (const std::exception& e) {
            std::cerr << "[uicoopa] UiAudio: failed to open audio device (" << e.what()
                      << "); UI sounds disabled\n";
            device_failed_ = true;
        }
    }

    /**
     * @brief Borrowed mode: plays through a host-owned engine and bus instead of owning
     * its own engine and device.
     *
     * Opens no device and creates no bus; `shared` and `ui_bus` must outlive this UiAudio.
     * available() is always true. The host owns the engine's lifecycle: borrowed mode
     * never calls engine.update() -- update() here only does UI-side bookkeeping -- so the
     * host must call AudioEngine::update() itself exactly once per frame (and pump its own
     * device or render_offline()). play() routes to `ui_bus` by name, so `ui_bus` must be a
     * bus created on `shared` (find_bus(ui_bus.name()) must resolve to it).
     */
    explicit UiAudio(coopa::sfx::core::AudioEngine& shared, coopa::sfx::mixer::MixerBus& ui_bus)
        : format_(shared.format()), engine_(&shared), ui_bus_(&ui_bus), borrowed_(true) {}

    UiAudio(const UiAudio&) = delete;
    UiAudio& operator=(const UiAudio&) = delete;

    /** @brief True when constructed from a host-owned engine (see the borrowing constructor). */
    bool borrowed() const { return borrowed_; }

    /**
     * @brief True unless a real device was requested (open_device=true) and genuinely failed
     * to open. Manual mode (open_device=false) is always available -- the caller is
     * responsible for pumping update()/render_offline() themselves.
     */
    bool available() const { return !device_failed_; }

    /**
     * @brief Resolves `sound_name` via SoundLibrary and plays it on the "UI" bus.
     *
     * Applies the SoundDef's gain (multiplied by `extra_gain`) and, if pitch_jitter is
     * nonzero, a per-call random pitch offset so repeated plays (e.g. rapid clicks) don't
     * sound machine-gunned. An unknown name or a decode failure is logged to stderr once
     * per sound name (not once per call) and returns an invalid VoiceHandle.
     */
    coopa::sfx::mixer::VoiceHandle play(const std::string& sound_name, float extra_gain = 1.0f) {
        if (!available()) return {};
        const SoundDef* def = SoundLibrary::instance().find(sound_name);
        if (!def) {
            warn_once_(sound_name, "unknown sound '" + sound_name + "'");
            return {};
        }

        coopa::sfx::core::PlayParams params;
        params.bus_name = ui_bus_->name();
        params.gain = def->gain * extra_gain;
        params.pitch = def->pitch_jitter > 0.0f ? jittered_pitch_(def->pitch_jitter) : 1.0f;

        try {
            return engine_->play(def->path, params);
        } catch (const std::exception& e) {
            warn_once_(sound_name, "failed to decode '" + def->path + "': " + e.what());
            return {};
        }
    }

    /**
     * @brief Main-thread per-frame tick. Owned mode forwards to AudioEngine::update();
     * borrowed mode never does (the host owns and updates the shared engine). Call once per frame.
     */
    void update(float delta_time) {
        if (!borrowed_) engine_->update(delta_time);
    }

    coopa::sfx::core::AudioEngine& engine() { return *engine_; }
    coopa::sfx::mixer::MixerBus& ui_bus() { return *ui_bus_; }

    /**
     * @brief Sets the "UI" bus's volume in decibels.
     *
     * Per mixer/mixer.h's documented concurrency scope: MixerBus volume is a plain,
     * non-atomic field, safe to set only between render() calls -- i.e. from the main
     * thread, not concurrently with a real AudioDevice callback. Calling this from the
     * same thread that calls update()/play() (the ordinary case) is safe.
     */
    void set_ui_volume_db(float db) { ui_bus_->set_volume_db(db); }

    /** @brief The process-wide active UiAudio, or nullptr if none has been set. Mirrors FontDefaults::font. */
    static UiAudio* active() { return active_; }
    /** @brief Sets (or clears, with nullptr) the process-wide active UiAudio instance. */
    static void set_active(UiAudio* audio) { active_ = audio; }

private:
    float jittered_pitch_(float jitter) {
        std::uniform_real_distribution<float> dist(1.0f - jitter, 1.0f + jitter);
        return dist(rng_);
    }

    void warn_once_(const std::string& sound_name, const std::string& message) {
        if (warned_.insert(sound_name).second) {
            std::cerr << "[uicoopa] UiAudio::play: " << message << "\n";
        }
    }

    coopa::sfx::data::AudioFormat format_;
    std::optional<coopa::sfx::core::AudioEngine> owned_engine_; /**< Engaged only in owned mode. */
    coopa::sfx::core::AudioEngine* engine_ = nullptr;           /**< owned_engine_ or the borrowed engine. */
    coopa::sfx::mixer::MixerBus* ui_bus_ = nullptr;
    bool borrowed_ = false;
    // Declared after owned_engine_ so it is destroyed first: the device callback
    // renders through engine_, which must outlive the device thread.
    std::unique_ptr<coopa::sfx::core::AudioDevice> device_;
    bool device_failed_ = false;
    std::mt19937 rng_{std::random_device{}()};
    std::unordered_set<std::string> warned_;

    inline static UiAudio* active_ = nullptr;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_HAS_AUDIO

#endif  // UICOOPA_AUDIO_UI_AUDIO_H
