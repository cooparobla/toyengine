/**
 * @file mixer.h
 * @brief Command, Event, and Mixer: the single audio-thread entry point
 * that drains queued voice commands, renders every active voice into its
 * target bus, walks the bus graph, and produces the final output block.
 *
 * Cross-thread safety: voice control (Play/Stop/Pause/Resume/
 * SetVoiceGain/SetVoicePitch/SetVoiceSpatial) is applied only via Command,
 * generation-checked against each Voice's own generation() so a stale
 * handle can never affect a slot that has since been reused. MixerBus
 * parameter setters (volume/mute/solo) and the bus graph itself are plain,
 * non-atomic fields read by render(): changing them while an AudioDevice
 * callback is concurrently rendering is not synchronized. The headless
 * tests drive Mixer synchronously (render_offline()-style), where this
 * does not arise.
 */

#ifndef SFXCOOPA_MIXER_MIXER_H
#define SFXCOOPA_MIXER_MIXER_H

#include <sfxcoopa/collections/ring_buffer.h>
#include <sfxcoopa/data/audio_format.h>
#include <sfxcoopa/mixer/bus.h>
#include <sfxcoopa/mixer/voice.h>
#include <sfxcoopa/spatial/panner.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace coopa {
namespace sfx {
namespace mixer {

/** @brief What a Command does. */
enum class CommandType {
    Play,
    Stop,
    Pause,
    Resume,
    SetVoiceGain,
    SetVoicePitch,
    SetVoiceSpatial
};

/**
 * @struct Command
 * @brief POD, voice-scoped control message pushed by the main thread and
 * applied by Mixer::render(). A flat struct rather than a literal C++
 * union (some fields, e.g. EarParams, aren't trivial enough to make a
 * union pleasant) -- only the fields relevant to `type` are meaningful,
 * so it's a tagged union in spirit. No std::function, no heap payloads:
 * `clip_data` is a raw pointer into an already-loaded AudioClip's PCM,
 * kept alive by the engine's clip cache and per-voice clip references for
 * as long as any voice references it.
 */
struct Command {
    CommandType type = CommandType::Stop;
    VoiceHandle voice;

    // Play:
    const float* clip_data = nullptr;
    uint64_t clip_frame_count = 0;
    uint32_t clip_channels = 0;
    uint32_t bus_index = 0;
    bool loop = false;
    uint64_t loop_start_frame = 0;
    uint64_t loop_end_frame = 0;
    bool spatialize = false;

    // SetVoiceGain / SetVoicePitch / Play's initial values:
    float gain = 1.0f;
    float pitch = 1.0f;

    // SetVoiceSpatial:
    coopa::sfx::spatial::EarParams spatial_target{};
};

/** @brief What an Event reports. */
enum class EventType {
    Finished, /**< Reached Free via natural end-of-clip or a completed stop() fade. */
    Looped,   /**< Wrapped from loop_end back to loop_start_frame at least once this block. */
    Stolen    /**< A Play command overwrote this voice while it was still active. */
};

/** @brief POD event pushed by Mixer::render(), drained by AudioEngine::update(dt) on the main thread. */
struct Event {
    EventType type = EventType::Finished;
    uint32_t voice_index = k_invalid_voice_index;
    uint32_t generation = 0;
};

/**
 * @class Mixer
 * @brief Owns the VoicePool, the bus graph rooted at "Master", and the
 * command/event rings. render() is the single audio-thread (or, in every
 * test here, synchronous offline) entry point.
 */
class Mixer {
public:
    Mixer(const coopa::sfx::data::AudioFormat& format, uint32_t max_voices = coopa::sfx::data::k_max_voices,
          uint32_t command_ring_capacity = 256, uint32_t event_ring_capacity = 256)
        : format_(format),
          voice_pool_(max_voices, format.sample_rate),
          commands_(command_ring_capacity),
          events_(event_ring_capacity) {
        create_bus("Master", nullptr);
    }

    /** @brief Creates a bus under `parent` (nullptr only valid for the very first call, which creates Master). */
    MixerBus& create_bus(const std::string& name, MixerBus* parent) {
        const uint32_t index = static_cast<uint32_t>(buses_.size());
        buses_.push_back(std::make_unique<MixerBus>(name, index, parent));
        MixerBus* bus = buses_.back().get();
        if (parent) parent->add_child(bus);
        bus_by_name_[name] = bus;
        if (max_frames_ > 0) bus->prepare(format_, max_frames_);
        return *bus;
    }

    MixerBus& master() { return *buses_[0]; }
    MixerBus* find_bus(const std::string& name) {
        auto it = bus_by_name_.find(name);
        return it != bus_by_name_.end() ? it->second : nullptr;
    }

    /** @brief Recomputes every bus's effective_mute() from current mute()/solo() flags. Call after changing either. */
    void update_bus_mutes() {
        std::vector<MixerBus*> all;
        all.reserve(buses_.size());
        for (auto& b : buses_) all.push_back(b.get());
        MixerBus::recompute_effective_mutes(all);
    }

    VoicePool& voice_pool() { return voice_pool_; }
    coopa::sfx::collections::RingBuffer<Command>& commands() { return commands_; }
    coopa::sfx::collections::RingBuffer<Event>& events() { return events_; }

    const coopa::sfx::data::AudioFormat& format() const { return format_; }

    /** @brief Allocates scratch buffers for every bus sized for up to `max_frames` frames per render() call. */
    void prepare(uint32_t max_frames) {
        max_frames_ = max_frames;
        for (auto& bus : buses_) bus->prepare(format_, max_frames);
    }

    /**
     * @brief The single render entry point: drains commands, renders every
     * active voice into its target bus, renders the bus graph post-order,
     * and writes the master bus's result to `out` (interleaved,
     * format().channels channels).
     */
    void render(float* out, uint32_t frames) {
        Command cmd;
        while (commands_.pop(cmd)) apply_command_(cmd);

        for (auto& bus : buses_) bus->clear_scratch(frames);

        for (uint32_t i = 0; i < voice_pool_.capacity(); ++i) {
            Voice& voice = voice_pool_.voice_at(i);
            if (voice.state() == VoiceState::Free) continue;

            const uint32_t loops_before = voice.loop_count();
            const uint32_t target = voice.target_bus_index();
            MixerBus* bus = (target < buses_.size()) ? buses_[target].get() : buses_[0].get();
            voice.render_add(bus->scratch(), frames, format_.channels);

            if (voice.loop_count() != loops_before) {
                events_.push(Event{EventType::Looped, i, voice.generation()});
            }
            if (voice.state() == VoiceState::Free) {
                events_.push(Event{EventType::Finished, i, voice.generation()});
            }
        }

        master().render(frames);

        const float* master_scratch = master().scratch();
        std::copy(master_scratch, master_scratch + static_cast<size_t>(frames) * format_.channels, out);
    }

private:
    void apply_command_(const Command& cmd) {
        if (cmd.voice.index >= voice_pool_.capacity()) return;
        Voice& voice = voice_pool_.voice_at(cmd.voice.index);

        if (cmd.type == CommandType::Play) {
            const bool was_active = voice.state() != VoiceState::Free;
            const uint32_t old_generation = voice.generation();
            voice.set_generation(cmd.voice.generation);
            voice.play(SourceRef{cmd.clip_data, cmd.clip_frame_count, cmd.clip_channels}, cmd.bus_index, cmd.gain,
                       cmd.pitch, cmd.loop, cmd.loop_start_frame, cmd.loop_end_frame);
            voice.set_spatialize(cmd.spatialize);
            if (was_active) events_.push(Event{EventType::Stolen, cmd.voice.index, old_generation});
            return;
        }

        // Every other command targets an already-established generation;
        // silently drop it if that generation has since moved on (the slot
        // was freed and reallocated to a different sound).
        if (voice.generation() != cmd.voice.generation) return;
        switch (cmd.type) {
            case CommandType::Stop: voice.stop(); break;
            case CommandType::Pause: voice.pause(); break;
            case CommandType::Resume: voice.resume(); break;
            case CommandType::SetVoiceGain: voice.set_gain_target(cmd.gain); break;
            case CommandType::SetVoicePitch: voice.set_pitch_target(cmd.pitch); break;
            case CommandType::SetVoiceSpatial: voice.set_spatial_target(cmd.spatial_target); break;
            case CommandType::Play: break;  // handled above
        }
    }

    coopa::sfx::data::AudioFormat format_;
    std::vector<std::unique_ptr<MixerBus>> buses_;
    std::unordered_map<std::string, MixerBus*> bus_by_name_;
    VoicePool voice_pool_;
    coopa::sfx::collections::RingBuffer<Command> commands_;
    coopa::sfx::collections::RingBuffer<Event> events_;
    uint32_t max_frames_ = 0;
};

}  // namespace mixer
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_MIXER_MIXER_H
