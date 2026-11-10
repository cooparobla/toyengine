/**
 * @file engine.h
 * @brief AudioEngine: the one object applications construct. Owns the
 * Mixer and clip cache; render_offline() is hardware-free so the whole
 * library is testable with no sound card, exactly as fast as
 * Mixer::render() itself.
 *
 * Two design points:
 *
 * 1. The constructor takes an AudioFormat, not a DeviceConfig -- DeviceConfig
 *    belongs to AudioDevice (core/device.h) and AudioEngine has no need to
 *    know about it at all. A device (when one exists) is wired up
 *    externally: `AudioDevice device(config, [&](float* out, uint32_t n) {
 *    engine.render_offline(out, n); });` -- so offline rendering is the
 *    engine's only render path, and the device adds nothing to it.
 * 2. load_clip(path) (single argument) is sidecar-aware -- it internally
 *    calls ClipImportSettings::load(path) -- while load_clip(path,
 *    settings) uses `settings` exactly as given, bypassing the sidecar
 *    entirely. This is the explicit-argument-wins tier from
 *    ClipImportSettings' own resolution order, realized as two overloads
 *    rather than a defaulted parameter, since a default argument can't
 *    distinguish "caller omitted this" from "caller explicitly wants {}".
 */

#ifndef SFXCOOPA_CORE_ENGINE_H
#define SFXCOOPA_CORE_ENGINE_H

#include <sfxcoopa/data/audio_clip.h>
#include <sfxcoopa/data/import_settings.h>
#include <sfxcoopa/mixer/mixer.h>
#include <sfxcoopa/spatial/listener.h>
#include <sfxcoopa/spatial/panner.h>

#include <coopa/debug/logger.h>
#include <coopa/event/signal.h>

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace coopa {
namespace sfx {
namespace core {

using coopa::sfx::mixer::VoiceHandle;
using coopa::sfx::mixer::MixerBus;

/**
 * @struct PlayParams
 * @brief Per-call play tuning. Loop points and whether a clip spatializes
 * are clip properties (set via its .import sidecar, resolved by
 * load_clip()'s sidecar-aware overload) rather than duplicated here --
 * PlayParams only covers what genuinely varies call to call.
 */
struct PlayParams {
    std::string bus_name;  /**< Empty = use the clip's own settings().default_bus. */
    float gain = 1.0f;
    float pitch = 1.0f;
    int priority = 0; /**< Higher steals over lower when the voice pool is exhausted. */
    std::optional<bool> loop; /**< Unset = the clip's own settings().loop (loop points always come from the clip). */
    /**
     * Unset = the clip's own settings().spatial. When set, replaces the clip's
     * spatial defaults (attenuation, cone, doppler, ...) for this voice; for
     * play_3d() the explicit `position` argument still wins over spatial->position.
     */
    std::optional<coopa::sfx::spatial::SpatialParams> spatial;
};

/**
 * @class AudioEngine
 * @brief The one object an application constructs. Ctor-init/dtor-shutdown
 * only, per house style -- no init()/shutdown() methods.
 */
class AudioEngine {
public:
    explicit AudioEngine(const coopa::sfx::data::AudioFormat& format,
                          uint32_t max_voices = coopa::sfx::data::k_max_voices, uint32_t max_block_frames = 4096)
        : format_(format), mixer_(format, max_voices), panner_for_compute_(format.sample_rate),
          max_block_frames_(max_block_frames) {
        mixer_.prepare(max_block_frames_);
    }

    /** @brief Plays a clip on its default (or PlayParams-overridden) bus, not spatialized. */
    VoiceHandle play(const std::string& clip_path, const PlayParams& params = {}) {
        return play_internal_(clip_path, params, false, glm::vec3(0.0f));
    }

    /**
     * @brief Plays a clip at a fixed 3D position, binaurally spatialized.
     * @throws std::runtime_error if the clip has more than one channel --
     *         set force_mono in its import settings first.
     */
    VoiceHandle play_3d(const std::string& clip_path, const glm::vec3& position, const PlayParams& params = {}) {
        return play_internal_(clip_path, params, true, position);
    }

    void stop(VoiceHandle handle) { push_simple_(handle, coopa::sfx::mixer::CommandType::Stop); }
    void pause(VoiceHandle handle) { push_simple_(handle, coopa::sfx::mixer::CommandType::Pause); }
    void resume(VoiceHandle handle) { push_simple_(handle, coopa::sfx::mixer::CommandType::Resume); }

    void set_voice_gain(VoiceHandle handle, float gain) {
        coopa::sfx::mixer::Command cmd{};
        cmd.type = coopa::sfx::mixer::CommandType::SetVoiceGain;
        cmd.voice = handle;
        cmd.gain = gain;
        mixer_.commands().push(cmd);
    }

    void set_voice_pitch(VoiceHandle handle, float pitch) {
        coopa::sfx::mixer::Command cmd{};
        cmd.type = coopa::sfx::mixer::CommandType::SetVoicePitch;
        cmd.voice = handle;
        cmd.pitch = pitch;
        mixer_.commands().push(cmd);
    }

    /** @brief Moves an actively-spatialized voice's source position and velocity (doppler); next update(dt). */
    void set_voice_motion(VoiceHandle handle, const glm::vec3& position, const glm::vec3& velocity) {
        auto it = spatialized_voices_.find(handle.index);
        if (it != spatialized_voices_.end() && it->second.generation == handle.generation) {
            it->second.params.position = position;
            it->second.params.velocity = velocity;
        }
    }

    /**
     * @brief True from play()/play_3d() until the voice's Finished/Stolen event
     * has been drained by update(dt) (or stop() was called and its fade has
     * finished and been drained). A stale or invalid handle reports false.
     */
    bool is_voice_active(VoiceHandle handle) const {
        if (!handle.is_valid()) return false;
        auto it = active_voices_.find(handle.index);
        return it != active_voices_.end() && it->second == handle.generation;
    }

    /** @brief The engine's mixing format (sample rate / channel count). */
    const coopa::sfx::data::AudioFormat& format() const { return format_; }

    /** @brief Moves an actively-spatialized voice's source position; takes effect on the next update(dt). */
    void set_voice_position(VoiceHandle handle, const glm::vec3& position) {
        auto it = spatialized_voices_.find(handle.index);
        if (it != spatialized_voices_.end() && it->second.generation == handle.generation) {
            it->second.params.position = position;
        }
    }

    coopa::sfx::mixer::MixerBus& master() { return mixer_.master(); }
    coopa::sfx::mixer::MixerBus& create_bus(const std::string& name, coopa::sfx::mixer::MixerBus& parent) {
        return mixer_.create_bus(name, &parent);
    }
    coopa::sfx::mixer::MixerBus* find_bus(const std::string& name) { return mixer_.find_bus(name); }

    void set_listener(const coopa::sfx::spatial::Listener& listener) { listener_ = listener; }

    /**
     * @brief Main-thread per-frame tick: drains events (firing Signals,
     * releasing per-voice clip references), reclaims finished voice pool
     * slots, refreshes bus solo/mute resolution, and recomputes spatial
     * targets for every actively-spatialized voice (see spatial/panner.h's
     * documented main-thread compute() cadence).
     */
    void update(float delta_time) {
        (void)delta_time;

        coopa::sfx::mixer::Event ev;
        while (mixer_.events().pop(ev)) {
            if (ev.type == coopa::sfx::mixer::EventType::Finished) {
                erase_if_generation_(ev.voice_index, ev.generation);
                on_voice_finished.emit(VoiceHandle{ev.voice_index, ev.generation});
            } else if (ev.type == coopa::sfx::mixer::EventType::Looped) {
                on_voice_looped.emit(VoiceHandle{ev.voice_index, ev.generation});
            } else if (ev.type == coopa::sfx::mixer::EventType::Stolen) {
                erase_if_generation_(ev.voice_index, ev.generation);
            }
        }

        mixer_.voice_pool().reclaim_finished();
        mixer_.update_bus_mutes();

        for (auto& kv : spatialized_voices_) {
            const uint32_t index = kv.first;
            const SpatialEntry& entry = kv.second;
            coopa::sfx::spatial::EarParams target =
                panner_for_compute_.compute(entry.params, listener_, format_.sample_rate);
            coopa::sfx::mixer::Command cmd{};
            cmd.type = coopa::sfx::mixer::CommandType::SetVoiceSpatial;
            cmd.voice = VoiceHandle{index, entry.generation};
            cmd.spatial_target = target;
            mixer_.commands().push(cmd);
        }
    }

    /** @brief Hardware-free render path: identical output to what a real AudioDevice callback would produce. */
    void render_offline(float* out, uint32_t frames) {
        if (frames > max_block_frames_) {
            throw std::runtime_error("[sfxcoopa] render_offline: frames exceeds max_block_frames configured at construction");
        }
        mixer_.render(out, frames);
    }

    /** @brief Sidecar-aware: loads (or returns the cached) clip using ClipImportSettings::load(path). */
    std::shared_ptr<coopa::sfx::data::AudioClip> load_clip(const std::string& path) {
        return load_clip(path, coopa::sfx::data::ClipImportSettings::load(path));
    }

    /** @brief Loads (or returns the cached) clip using `settings` exactly as given -- no sidecar lookup. */
    std::shared_ptr<coopa::sfx::data::AudioClip> load_clip(const std::string& path,
                                                            const coopa::sfx::data::ClipImportSettings& settings) {
        const std::string key = cache_key_(path, settings);
        auto it = clip_cache_.find(key);
        if (it != clip_cache_.end()) return it->second;
        auto clip = std::make_shared<coopa::sfx::data::AudioClip>(
            coopa::sfx::data::AudioClip::load(path, format_, settings));
        clip_cache_[key] = clip;
        return clip;
    }

    coopa::event::Signal<VoiceHandle> on_voice_finished; /**< Emitted from update(), main thread. */
    coopa::event::Signal<VoiceHandle> on_voice_looped;

private:
    struct SpatialEntry {
        coopa::sfx::spatial::SpatialParams params;
        uint32_t generation = 0;
    };

    static std::string cache_key_(const std::string& path, const coopa::sfx::data::ClipImportSettings& s) {
        // Only fields that change the decoded bytes belong in the key --
        // volume/pitch/spatial/etc. are applied at play time, not bake time.
        return path + "|" + std::to_string(static_cast<int>(s.load_type)) + "|" + (s.force_mono ? "1" : "0") + "|" +
               std::to_string(s.sample_rate_override);
    }

    /**
     * Drops per-voice bookkeeping for `index` only if it still belongs to
     * `generation` -- a Finished/Stolen event for an old occupant must not
     * erase the bookkeeping of a newer voice already allocated in the same slot.
     */
    void erase_if_generation_(uint32_t index, uint32_t generation) {
        auto active = active_voices_.find(index);
        if (active != active_voices_.end() && active->second != generation) return;
        if (active != active_voices_.end()) active_voices_.erase(active);
        voice_clip_refs_.erase(index);
        auto spatial = spatialized_voices_.find(index);
        if (spatial != spatialized_voices_.end() && spatial->second.generation == generation) {
            spatialized_voices_.erase(spatial);
        }
    }

    void push_simple_(VoiceHandle handle, coopa::sfx::mixer::CommandType type) {
        coopa::sfx::mixer::Command cmd{};
        cmd.type = type;
        cmd.voice = handle;
        mixer_.commands().push(cmd);
    }

    VoiceHandle play_internal_(const std::string& clip_path, const PlayParams& params, bool spatialize_3d,
                                const glm::vec3& position) {
        std::shared_ptr<coopa::sfx::data::AudioClip> clip = load_clip(clip_path);
        const coopa::sfx::data::ClipImportSettings& clip_settings = clip->settings();

        if (spatialize_3d && clip->format().channels > 1) {
            throw std::runtime_error("[sfxcoopa] play_3d: clip '" + clip_path +
                                      "' is stereo; set force_mono in its import settings");
        }

        const std::string bus_name = params.bus_name.empty() ? clip_settings.default_bus : params.bus_name;
        coopa::sfx::mixer::MixerBus* bus = find_bus(bus_name);
        if (!bus) bus = &master();

        const bool spatialize = spatialize_3d || clip_settings.spatialize;
        VoiceHandle handle = mixer_.voice_pool().allocate(params.priority, next_started_at_++);
        // Until the audio thread applies the Play command below, the slot's
        // Voice still looks Free (or still holds a stolen voice); keep
        // reclaim_finished() from handing the slot out again in the meantime.
        mixer_.voice_pool().mark_pending(handle);
        active_voices_[handle.index] = handle.generation;

        coopa::sfx::mixer::Command cmd{};
        cmd.type = coopa::sfx::mixer::CommandType::Play;
        cmd.voice = handle;
        cmd.clip_data = clip->data();
        cmd.clip_frame_count = clip->frame_count();
        cmd.clip_channels = clip->format().channels;
        cmd.bus_index = bus->index();
        cmd.gain = params.gain;
        cmd.pitch = params.pitch;
        cmd.loop = params.loop.value_or(clip_settings.loop);
        cmd.loop_start_frame = clip_settings.loop_start_frame;
        cmd.loop_end_frame = clip_settings.loop_end_frame;
        cmd.spatialize = spatialize;
        mixer_.commands().push(cmd);

        voice_clip_refs_[handle.index] = clip;  // keep the clip alive for as long as this voice references it

        if (spatialize) {
            SpatialEntry entry{};
            entry.params = params.spatial ? *params.spatial : clip_settings.spatial;
            if (spatialize_3d || !params.spatial) entry.params.position = position;
            entry.generation = handle.generation;
            spatialized_voices_[handle.index] = entry;

            // Push an initial target immediately so the very first render()
            // block isn't silent while waiting for the next update(dt).
            coopa::sfx::spatial::EarParams initial =
                panner_for_compute_.compute(entry.params, listener_, format_.sample_rate);
            coopa::sfx::mixer::Command spatial_cmd{};
            spatial_cmd.type = coopa::sfx::mixer::CommandType::SetVoiceSpatial;
            spatial_cmd.voice = handle;
            spatial_cmd.spatial_target = initial;
            mixer_.commands().push(spatial_cmd);
        }

        return handle;
    }

    coopa::debug::Logger logger_{"AudioEngine"};
    coopa::sfx::data::AudioFormat format_;
    coopa::sfx::mixer::Mixer mixer_;
    coopa::sfx::spatial::Listener listener_{};
    coopa::sfx::spatial::BinauralPanner panner_for_compute_; /**< Used only for compute(); never process()/set_target(). */
    uint32_t max_block_frames_;
    uint64_t next_started_at_ = 0;

    std::unordered_map<std::string, std::shared_ptr<coopa::sfx::data::AudioClip>> clip_cache_;
    std::unordered_map<uint32_t, std::shared_ptr<coopa::sfx::data::AudioClip>> voice_clip_refs_;
    std::unordered_map<uint32_t, SpatialEntry> spatialized_voices_;
    std::unordered_map<uint32_t, uint32_t> active_voices_; /**< slot index -> generation of its live voice. */
};

}  // namespace core
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_CORE_ENGINE_H
