/**
 * @file audio_clip.h
 * @brief Fully decoded, interleaved f32 PCM held in memory for the
 * DecompressOnLoad path.
 */

#ifndef SFXCOOPA_DATA_AUDIO_CLIP_H
#define SFXCOOPA_DATA_AUDIO_CLIP_H

#include <sfxcoopa/data/decoder.h>
#include <sfxcoopa/data/import_settings.h>
#include <sfxcoopa/data/audio_format.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace coopa {
namespace sfx {
namespace data {

/**
 * @class AudioClip
 * @brief Owns a clip's fully-decoded PCM data at a chosen engine format.
 *
 * Constructed only via load(): a Decoder is opened at the source's own
 * native channel count and the requested rate, so miniaudio's own f32-at-
 * matching-rate passthrough (proven bit-exact in Decoder's tests) is what
 * makes the AudioClip-level decode a straight copy whenever the source
 * already matches the engine's rate -- there is no separate "fast path"
 * branch here to get wrong, it falls out of always asking Decoder for
 * exactly the channel count it needs. force_mono is the one case Decoder
 * can't do alone: it's applied as an explicit downmix pass afterward.
 *
 * Non-copyable (owns a potentially large PCM buffer), movable. Shared
 * across voices via std::shared_ptr<AudioClip>, cached by path+settings in
 * the engine (see AudioEngine::load_clip in core/engine.h).
 */
class AudioClip {
public:
    AudioClip(const AudioClip&) = delete;
    AudioClip& operator=(const AudioClip&) = delete;
    AudioClip(AudioClip&&) = default;
    AudioClip& operator=(AudioClip&&) = default;

    /**
     * @brief Decodes `path` fully into memory at `engine_format`'s sample
     * rate (or settings.sample_rate_override, if nonzero) and the source's
     * own channel count (or 1, if settings.force_mono).
     *
     * @throws std::runtime_error if settings.spatialize is true but the
     *         clip would end up with more than one channel (spatialization
     *         is only defined for a mono point source) -- set force_mono
     *         in the import settings to resolve this.
     */
    static AudioClip load(const std::string& path, const AudioFormat& engine_format,
                           const ClipImportSettings& settings = {}) {
        const uint32_t target_rate = settings.sample_rate_override != 0 ? settings.sample_rate_override
                                                                         : engine_format.sample_rate;

        uint32_t source_channels;
        {
            // Any channel count works for probing -- Decoder always learns
            // the source's real channel count via its own internal probe
            // regardless of what output channel count we request here.
            Decoder probe(path, AudioFormat{target_rate, 2});
            source_channels = probe.source_format().channels;
        }

        const bool needs_manual_downmix = settings.force_mono && source_channels > 1;
        const uint32_t final_channels = settings.force_mono ? 1 : source_channels;
        const uint32_t decode_channels = needs_manual_downmix ? source_channels : final_channels;

        if (settings.spatialize && final_channels > 1) {
            throw std::runtime_error("[sfxcoopa] AudioClip::load: clip '" + path +
                                      "' is stereo; set force_mono in its import settings");
        }

        Decoder dec(path, AudioFormat{target_rate, decode_channels});
        const uint64_t total = dec.total_frames();
        std::vector<float> raw(static_cast<size_t>(total) * decode_channels);
        const uint64_t got = dec.read_frames(raw.data(), total);
        raw.resize(static_cast<size_t>(got) * decode_channels);

        AudioClip clip;
        clip.settings_ = settings;
        clip.format_ = AudioFormat{target_rate, final_channels};

        if (needs_manual_downmix) {
            clip.pcm_.resize(got);
            const float inv_n = 1.0f / static_cast<float>(decode_channels);
            for (uint64_t i = 0; i < got; ++i) {
                float sum = 0.0f;
                for (uint32_t c = 0; c < decode_channels; ++c) {
                    sum += raw[static_cast<size_t>(i) * decode_channels + c];
                }
                clip.pcm_[static_cast<size_t>(i)] = sum * inv_n;
            }
        } else {
            clip.pcm_ = std::move(raw);
        }

        return clip;
    }

    /** @brief Number of frames (not interleaved samples) of decoded PCM. */
    uint64_t frame_count() const { return format_.channels ? pcm_.size() / format_.channels : 0; }

    /** @brief The format this clip's PCM is stored at. */
    const AudioFormat& format() const { return format_; }

    /** @brief The import settings this clip was loaded with. */
    const ClipImportSettings& settings() const { return settings_; }

    /** @brief Raw interleaved PCM data, frame_count() * format().channels floats. */
    const float* data() const { return pcm_.data(); }

private:
    AudioClip() = default;

    std::vector<float> pcm_;
    AudioFormat format_{};
    ClipImportSettings settings_{};
};

}  // namespace data
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_DATA_AUDIO_CLIP_H
