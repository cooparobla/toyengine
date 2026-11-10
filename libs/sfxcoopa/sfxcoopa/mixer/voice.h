/**
 * @file voice.h
 * @brief A single playing sound: source, playback cursor, ramped
 * gain/pitch, optional binaural panning, and the fixed-size pool that owns
 * all of them.
 *
 * Voice allocation is main-thread-only (see AudioEngine::play() in
 * core/engine.h): VoicePool::allocate() -- called only from the main
 * thread -- assigns a slot and bumps that slot's generation itself. The
 * audio thread (Mixer::render(), mixer/mixer.h) never allocates a slot and
 * never bumps a generation; it only ever transitions a Voice's own state_
 * toward Free as playback completes or fades out. Reclaiming a finished
 * slot back to the free list (VoicePool::reclaim_finished()) is therefore
 * also main-thread-only, driven by observing Voice::state(), typically
 * once per AudioEngine::update(dt) tick.
 */

#ifndef SFXCOOPA_MIXER_VOICE_H
#define SFXCOOPA_MIXER_VOICE_H

#include <sfxcoopa/data/audio_format.h>
#include <sfxcoopa/spatial/panner.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace coopa {
namespace sfx {
namespace mixer {

/** @brief Sentinel VoiceHandle::index meaning "no voice". */
inline constexpr uint32_t k_invalid_voice_index = UINT32_MAX;

/**
 * @struct VoiceHandle
 * @brief A generational reference to a pool slot: a stale handle (one
 * whose generation no longer matches the slot's current generation,
 * because the slot was freed and reallocated to a different sound since)
 * can never affect the wrong, recycled voice.
 */
struct VoiceHandle {
    uint32_t index = k_invalid_voice_index;
    uint32_t generation = 0;
    bool is_valid() const { return index != k_invalid_voice_index; }
};

/** @brief Where a Voice currently is in its lifecycle. */
enum class VoiceState {
    Free,
    Playing,
    Paused,
    Stopping /**< Fading to silence (explicit stop() or natural end-of-clip), about to become Free. */
};

/**
 * @struct SourceRef
 * @brief What a Voice plays from: a non-owning reference to an AudioClip's
 * PCM. Kept as its own struct so a streaming source (the planned
 * AudioStream, see plans/) can be added without touching render_add's
 * mixing logic -- only this struct and the fetch it feeds would change.
 */
struct SourceRef {
    const float* clip_data = nullptr; /**< Interleaved PCM; non-owning, must outlive the Voice's playback. */
    uint64_t clip_frame_count = 0;
    uint32_t channels = 0;
};

/**
 * @class Voice
 * @brief One playing sound. render_add() mixes additively into a bus's
 * scratch buffer. Gain and pitch changes are *targets* the voice ramps
 * toward rather than applying instantaneously, so they never pop. Spatial
 * targets (set_spatial_target()) are handed straight to the panner and held
 * until the next one arrives, typically once per AudioEngine::update(dt);
 * their doppler pitch_scale ramps like pitch and multiplies the playback
 * rate whenever the panner is active (spatialized mono source).
 *
 * Two independent smoothing mechanisms, deliberately different because
 * they solve different problems: gain/pitch use a per-control-block
 * exponential approach toward their targets (cheap, recomputed only every
 * k_control_block_frames samples, converges over a handful of blocks);
 * the stop/end-of-clip fade uses a per-sample *linear* ramp to exactly
 * zero over a fixed ~5ms, because "reaches exactly zero in a bounded time"
 * is what lets the voice transition to Free deterministically.
 */
class Voice {
public:
    explicit Voice(uint32_t sample_rate) : sample_rate_(sample_rate), panner_(sample_rate) {}

    /** @brief Starts playback from the beginning of `source`, resetting all ramp/fade state. */
    void play(const SourceRef& source, uint32_t target_bus_index, float gain, float pitch, bool loop,
              uint64_t loop_start_frame, uint64_t loop_end_frame) {
        source_ = source;
        target_bus_index_ = target_bus_index;
        gain_current_ = gain_target_ = gain;
        pitch_current_ = pitch_target_ = pitch;
        doppler_current_ = doppler_target_ = 1.0f;
        loop_ = loop;
        loop_start_frame_ = loop_start_frame;
        loop_end_frame_ = loop_end_frame;
        cursor_ = 0.0;
        fade_gain_ = 1.0f;
        block_phase_ = 0;
        loop_count_ = 0;
        spatialize_ = false;
        state_ = VoiceState::Playing;
    }

    /** @brief Begins a ~5ms fade to Free rather than cutting playback immediately. */
    void stop() {
        if (state_ == VoiceState::Playing || state_ == VoiceState::Paused) state_ = VoiceState::Stopping;
    }

    void pause() {
        if (state_ == VoiceState::Playing) state_ = VoiceState::Paused;
    }

    void resume() {
        if (state_ == VoiceState::Paused) state_ = VoiceState::Playing;
    }

    VoiceState state() const { return state_; }
    uint32_t target_bus_index() const { return target_bus_index_; }

    /**
     * @brief The generation this Voice was most recently (re)established
     * under. Set by whoever applies a Play command (see mixer/mixer.h);
     * commands whose generation doesn't match are stale and dropped
     * without affecting this Voice.
     */
    uint32_t generation() const { return generation_; }
    void set_generation(uint32_t generation) { generation_ = generation; }

    /** @brief Incremented each time playback wraps at loop_end back to loop_start_frame. */
    uint32_t loop_count() const { return loop_count_; }

    void set_gain_target(float gain) { gain_target_ = gain; }
    void set_pitch_target(float pitch) { pitch_target_ = pitch; }

    /** @brief Enables binaural panning for this voice; only applied when the source is mono (see AudioClip's spatialize guard). */
    void set_spatialize(bool enabled) { spatialize_ = enabled; }

    /** @brief Updates the panning target this voice ramps toward; see BinauralPanner::set_target(). */
    void set_spatial_target(const coopa::sfx::spatial::EarParams& target) {
        panner_.set_target(target);
        doppler_target_ = target.pitch_scale;
    }

    /**
     * @brief Additively mixes up to `frames` frames into `bus_scratch`
     * (interleaved, bus_channels channels). No-op for Free or Paused
     * voices. Gain/pitch ramp steps are recomputed at each
     * k_control_block_frames boundary; per-sample state (cursor, fade)
     * advances every frame.
     */
    void render_add(float* bus_scratch, uint32_t frames, uint32_t bus_channels) {
        if (state_ == VoiceState::Free || state_ == VoiceState::Paused) return;

        uint32_t produced = 0;
        while (produced < frames && state_ != VoiceState::Free) {
            if (block_phase_ == 0) {
                gain_current_ += (gain_target_ - gain_current_) * k_ramp_coefficient;
                pitch_current_ += (pitch_target_ - pitch_current_) * k_ramp_coefficient;
                doppler_current_ += (doppler_target_ - doppler_current_) * k_ramp_coefficient;
            }
            const uint32_t block =
                std::min(frames - produced, coopa::sfx::data::k_control_block_frames - block_phase_);
            render_block_(bus_scratch + static_cast<size_t>(produced) * bus_channels, block, bus_channels);
            produced += block;
            block_phase_ = (block_phase_ + block) % coopa::sfx::data::k_control_block_frames;
        }
    }

private:
    static constexpr float k_ramp_coefficient = 0.3f;

    void render_block_(float* out, uint32_t frames, uint32_t bus_channels) {
        for (uint32_t i = 0; i < frames; ++i) {
            if (state_ == VoiceState::Free) break;

            float fetched[2] = {0.0f, 0.0f};
            for (uint32_t c = 0; c < source_.channels && c < 2; ++c) fetched[c] = sample_channel_(cursor_, c);

            const float total_gain = gain_current_ * fade_gain_;

            if (spatialize_ && source_.channels == 1) {
                float stereo[2];
                panner_.process(fetched, stereo, 1);
                for (uint32_t c = 0; c < bus_channels && c < 2; ++c) out[i * bus_channels + c] += stereo[c] * total_gain;
            } else if (source_.channels == 1) {
                const float s = fetched[0] * total_gain;
                for (uint32_t c = 0; c < bus_channels; ++c) out[i * bus_channels + c] += s;
            } else {
                for (uint32_t c = 0; c < bus_channels; ++c) {
                    out[i * bus_channels + c] += fetched[c % source_.channels] * total_gain;
                }
            }

            advance_cursor_();

            if (state_ == VoiceState::Stopping) {
                fade_gain_ -= fade_step_();
                if (fade_gain_ <= 0.0f) {
                    fade_gain_ = 0.0f;
                    state_ = VoiceState::Free;
                }
            }
        }
    }

    void advance_cursor_() {
        const float doppler = (spatialize_ && source_.channels == 1) ? doppler_current_ : 1.0f;
        cursor_ += static_cast<double>(pitch_current_ * doppler);
        const uint64_t total = source_.clip_frame_count;
        if (loop_) {
            const uint64_t effective_end = (loop_end_frame_ == 0) ? total : loop_end_frame_;
            if (effective_end > 0 && cursor_ >= static_cast<double>(effective_end)) {
                cursor_ = static_cast<double>(loop_start_frame_) + (cursor_ - static_cast<double>(effective_end));
                ++loop_count_;
            }
        } else if (cursor_ >= static_cast<double>(total) && state_ == VoiceState::Playing) {
            state_ = VoiceState::Stopping;  // natural end: fade rather than cut, same as an explicit stop().
        }
    }

    float sample_channel_(double cursor, uint32_t channel) const {
        const uint64_t total = source_.clip_frame_count;
        if (total == 0) return 0.0f;
        uint64_t i0 = static_cast<uint64_t>(cursor);
        if (i0 >= total) i0 = total - 1;
        const uint64_t i1 = (i0 + 1 < total) ? i0 + 1 : i0;
        const float frac = static_cast<float>(cursor - static_cast<double>(i0));
        const float a = source_.clip_data[i0 * source_.channels + channel];
        const float b = source_.clip_data[i1 * source_.channels + channel];
        return a + (b - a) * frac;
    }

    float fade_step_() const {
        const uint32_t fade_frames = std::max<uint32_t>(1, sample_rate_ * 5 / 1000);
        return 1.0f / static_cast<float>(fade_frames);
    }

    uint32_t sample_rate_;
    SourceRef source_{};
    uint32_t target_bus_index_ = 0;
    VoiceState state_ = VoiceState::Free;

    double cursor_ = 0.0;
    bool loop_ = false;
    uint64_t loop_start_frame_ = 0;
    uint64_t loop_end_frame_ = 0;

    float gain_current_ = 1.0f, gain_target_ = 1.0f;
    float pitch_current_ = 1.0f, pitch_target_ = 1.0f;
    float doppler_current_ = 1.0f, doppler_target_ = 1.0f; /**< EarParams::pitch_scale, applied only while panning. */
    float fade_gain_ = 1.0f;
    uint32_t block_phase_ = 0;
    uint32_t generation_ = 0;
    uint32_t loop_count_ = 0;

    bool spatialize_ = false;
    coopa::sfx::spatial::BinauralPanner panner_;
};

/**
 * @class VoicePool
 * @brief Fixed-size array of Voices with main-thread-only allocation.
 *
 * allocate() pulls from a free list when one is available; when exhausted,
 * it steals the lowest-priority (ties broken by oldest) currently-playing
 * voice. Stealing takes over the slot immediately via Voice::play() rather
 * than fading the stolen sound out first (the Mixer reports a Stolen event
 * for the old occupant). Exhaustion never throws; it always hands back a
 * valid handle for the new sound.
 */
class VoicePool {
public:
    VoicePool(uint32_t capacity, uint32_t sample_rate) {
        slots_.reserve(capacity);
        for (uint32_t i = 0; i < capacity; ++i) slots_.emplace_back(sample_rate);
        for (uint32_t i = capacity; i-- > 0;) free_list_.push_back(i);
    }

    /** @brief Allocates a slot (from the free list, or by stealing) and bumps its generation. Main-thread only. */
    VoiceHandle allocate(int priority, uint64_t started_at) {
        uint32_t index;
        if (!free_list_.empty()) {
            index = free_list_.back();
            free_list_.pop_back();
        } else {
            index = find_steal_victim_();
        }
        Slot& slot = slots_[index];
        slot.generation++;
        slot.in_use = true;
        slot.pending = false;
        slot.priority = priority;
        slot.started_at = started_at;
        return VoiceHandle{index, slot.generation};
    }

    /** @brief True if `handle` refers to the slot's current occupancy (not stale, not out of range). */
    bool is_valid(VoiceHandle handle) const {
        return handle.is_valid() && handle.index < slots_.size() && slots_[handle.index].in_use &&
               slots_[handle.index].generation == handle.generation;
    }

    /** @brief The Voice for `handle`, or nullptr if the handle is stale/invalid. */
    Voice* get(VoiceHandle handle) { return is_valid(handle) ? &slots_[handle.index].voice : nullptr; }

    /**
     * @brief Marks a freshly allocated slot as awaiting its Play command.
     *
     * Until the audio thread applies that command (stamping the Voice with
     * the handle's generation), the slot's Voice still reports Free -- or is
     * still rendering the voice being stolen -- so reclaim_finished() must
     * not hand the slot out again. Without this, two play() calls separated
     * only by an update(dt) (no render in between) would land on the same
     * slot and the first sound would be silently overwritten. Main-thread only.
     */
    void mark_pending(VoiceHandle handle) {
        if (is_valid(handle)) slots_[handle.index].pending = true;
    }

    /** @brief Scans for slots whose Voice has reached Free and returns them to the free list. Main-thread only. */
    void reclaim_finished() {
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            if (slot.pending) {
                if (slot.voice.generation() != slot.generation) continue;  // Play not applied yet
                slot.pending = false;
            }
            if (slot.in_use && slot.voice.state() == VoiceState::Free) {
                slot.in_use = false;
                free_list_.push_back(i);
            }
        }
    }

    /** @brief Direct index access for the audio-thread render loop, which iterates every slot each block. */
    Voice& voice_at(uint32_t index) { return slots_[index].voice; }

    uint32_t capacity() const { return static_cast<uint32_t>(slots_.size()); }

private:
    struct Slot {
        explicit Slot(uint32_t sample_rate) : voice(sample_rate) {}
        Voice voice;
        uint32_t generation = 0;
        bool in_use = false;
        bool pending = false; /**< Allocated, but its Play command hasn't been applied yet (see mark_pending). */
        int priority = 0;
        uint64_t started_at = 0;
    };

    uint32_t find_steal_victim_() const {
        uint32_t victim = 0;
        bool found = false;
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i].in_use) continue;
            if (!found || slots_[i].priority < slots_[victim].priority ||
                (slots_[i].priority == slots_[victim].priority && slots_[i].started_at < slots_[victim].started_at)) {
                victim = i;
                found = true;
            }
        }
        return victim;
    }

    std::vector<Slot> slots_;
    std::vector<uint32_t> free_list_;
};

}  // namespace mixer
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_MIXER_VOICE_H
