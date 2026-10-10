/**
 * @file audio_system.h
 * @brief The engine's audio: one sfxcoopa AudioEngine, its buses, and the sound card.
 *
 *   Master
 *    ├─ Music   (MusicPlayer: background tracks, crossfades, playlists -- music())
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

class MusicPlayer;

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
    explicit AudioSystem(const AudioSystemOptions& opt = {});

    ~AudioSystem();

    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    coopa::sfx::core::AudioEngine& engine() { return engine_; }

    /** @brief Background music on the Music bus (see music_player.h). */
    MusicPlayer& music() { return *music_player_; }

    /** @brief The running Engine's audio (set by toy::core::Engine), for components like VolumeBinding. */
    static AudioSystem* active() { return active_ptr_(); }
    static void set_active(AudioSystem* a) { active_ptr_() = a; }
#ifdef UICOOPA_HAS_AUDIO
    coopa::ui::UiAudio* ui() { return ui_.get(); }
#endif
    /** @brief True when a real (non-null) sound card is open. */
    bool has_output() const { return device_ && !null_device_; }

    /** @brief A bus by name (Master, Music, SFX, UI, or any created later). */
    coopa::sfx::mixer::MixerBus* bus(const std::string& name);

    /** @brief Linear volume (0..1+) of a bus; false if no such bus. */
    bool set_bus_volume(const std::string& name, float linear);
    float bus_volume(const std::string& name);

    /**
     * @brief Fire-and-forget 2D sound (a pickup, a hit): `path` is resolved against the asset
     *        roots. Returns an invalid handle (and logs once) if the clip can't load.
     */
    coopa::sfx::mixer::VoiceHandle play_oneshot(const std::string& path, const std::string& bus_name = k_bus_sfx,
                                                float gain = 1.0f, float pitch = 1.0f);

    /** @brief play_oneshot() at a world position (mono clips; stereo plays 2D). */
    coopa::sfx::mixer::VoiceHandle play_oneshot_3d(const std::string& path, const glm::vec3& position,
                                                   const std::string& bus_name = k_bus_sfx, float gain = 1.0f);

    /** @brief Freezes all audio output (game paused); resume_all() continues where it stopped. */
    void pause_all() { paused_ = true; }
    void resume_all() { paused_ = false; }
    bool paused() const { return paused_; }

    /** @brief Stops every one-shot this system started and the music (AudioSources stop with their objects). */
    void stop_all();

    /** @brief Stops the one-shots only: a scene change, where the music carries on. */
    void stop_oneshots();

    /** @brief Where the listener was put last update() (the camera's position when it drove it). */
    const glm::vec3& listener_position() const { return listener_position_; }

    /**
     * @brief Per frame, after the scene updated: the listener follows an AudioListener if one
     *        pushed this frame, else `camera_world` (when given); then the engine's main-thread
     *        bookkeeping (finished voices, clip GC, signals).
     */
    void update(float dt, const glm::mat4* camera_world);

private:
    static AudioSystem*& active_ptr_();
    coopa::sfx::mixer::VoiceHandle track_(coopa::sfx::mixer::VoiceHandle h);
    void warn_(const std::string& path, const std::string& why);

    coopa::sfx::data::AudioFormat format_;
    coopa::sfx::core::AudioEngine engine_;
    coopa::sfx::mixer::MixerBus* music_ = nullptr;
    coopa::sfx::mixer::MixerBus* sfx_ = nullptr;
    coopa::sfx::mixer::MixerBus* ui_bus_ = nullptr;
    std::unique_ptr<MusicPlayer> music_player_;
    glm::vec3 listener_position_{0.0f};
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
