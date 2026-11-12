/**
 * @file decoder.h
 * @brief RAII wrapper over ma_decoder: bytes -> f32 PCM at a fixed engine
 * sample rate/channel count, regardless of the source file's own format.
 */

#ifndef SFXCOOPA_DATA_DECODER_H
#define SFXCOOPA_DATA_DECODER_H

#include <sfxcoopa/util/error.h>
#include <sfxcoopa/data/audio_format.h>

#include <miniaudio/miniaudio.h>

#include <cstdint>
#include <string>

namespace coopa {
namespace sfx {
namespace data {

/** @brief Maps a miniaudio sample format to sfxcoopa's SampleFormat. */
inline SampleFormat sample_format_from_ma(ma_format fmt) {
    switch (fmt) {
        case ma_format_u8: return SampleFormat::U8;
        case ma_format_s16: return SampleFormat::S16;
        case ma_format_s24: return SampleFormat::S24;
        case ma_format_s32: return SampleFormat::S32;
        case ma_format_f32: return SampleFormat::F32;
        default: return SampleFormat::F32;
    }
}

/** @brief The source file's own format, before any rate/channel conversion. */
struct SourceFormat {
    SampleFormat sample_format = SampleFormat::F32;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
};

/**
 * @class Decoder
 * @brief RAII over ma_decoder, always producing f32 PCM at a caller-chosen
 * sample rate/channel count -- the format zoo (u8/s16/s24/s32/f32, any
 * source rate, mono/stereo/etc.) stops here.
 *
 * Constructing a Decoder briefly opens a second, default-configured
 * ma_decoder purely to query the source's own native format via
 * ma_decoder_get_data_format() before the real, format-converting decoder
 * is opened -- this is what source_format() reports, distinct from
 * format() (the engine-rate output this Decoder actually produces). Tests
 * need this distinction to tell "decoded correctly" apart from "resampled
 * correctly", and it's how AudioClip learns the source's channel count.
 */
class Decoder {
public:
    /** @brief Opens a wav/mp3 file from a filesystem path (FLAC is compiled out via MA_NO_FLAC). */
    Decoder(const std::string& path, const AudioFormat& output_format) {
        probe_source_format_([&](ma_decoder* probe, const ma_decoder_config* cfg) {
            return ma_decoder_init_file(path.c_str(), cfg, probe);
        });
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, output_format.channels, output_format.sample_rate);
        SFX_MA_CHECK(ma_decoder_init_file(path.c_str(), &cfg, &decoder_));
        format_ = output_format;
        initialized_ = true;
    }

    /** @brief Opens a wav/mp3 file already resident in memory. The memory must outlive this Decoder. */
    Decoder(const void* data, size_t size, const AudioFormat& output_format) {
        probe_source_format_([&](ma_decoder* probe, const ma_decoder_config* cfg) {
            return ma_decoder_init_memory(data, size, cfg, probe);
        });
        ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, output_format.channels, output_format.sample_rate);
        SFX_MA_CHECK(ma_decoder_init_memory(data, size, &cfg, &decoder_));
        format_ = output_format;
        initialized_ = true;
    }

    ~Decoder() {
        if (initialized_) ma_decoder_uninit(&decoder_);
    }

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;

    /**
     * @brief Reads up to `frame_count` frames of interleaved f32 PCM at format().
     * @return The number of frames actually read; less than requested at end of stream.
     */
    uint64_t read_frames(float* out, uint64_t frame_count) {
        ma_uint64 frames_read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(&decoder_, out, frame_count, &frames_read);
        if (result != MA_SUCCESS && result != MA_AT_END) SFX_MA_CHECK(result);
        return frames_read;
    }

    /** @brief Seeks the read cursor to the given frame index, in format()'s sample rate. */
    void seek_to_frame(uint64_t frame) {
        SFX_MA_CHECK(ma_decoder_seek_to_pcm_frame(&decoder_, frame));
    }

    /** @brief Total frame count at format()'s sample rate. */
    uint64_t total_frames() const {
        ma_uint64 total = 0;
        SFX_MA_CHECK(ma_decoder_get_length_in_pcm_frames(const_cast<ma_decoder*>(&decoder_), &total));
        return total;
    }

    /** @brief The format this Decoder actually produces (always f32, at the requested rate/channels). */
    const AudioFormat& format() const { return format_; }

    /** @brief The source file's own format, before rate/channel/sample-format conversion. */
    const SourceFormat& source_format() const { return source_format_; }

private:
    template <typename InitFn>
    void probe_source_format_(InitFn init_fn) {
        ma_decoder probe;
        ma_decoder_config probe_cfg = ma_decoder_config_init_default();  // unknown/0/0 => native passthrough
        SFX_MA_CHECK(init_fn(&probe, &probe_cfg));
        ma_format native_format;
        ma_uint32 native_channels = 0;
        ma_uint32 native_rate = 0;
        SFX_MA_CHECK(ma_decoder_get_data_format(&probe, &native_format, &native_channels, &native_rate, nullptr, 0));
        source_format_.sample_format = sample_format_from_ma(native_format);
        source_format_.channels = native_channels;
        source_format_.sample_rate = native_rate;
        ma_decoder_uninit(&probe);
    }

    ma_decoder decoder_{};
    AudioFormat format_{};
    SourceFormat source_format_{};
    bool initialized_ = false;
};

}  // namespace data
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_DATA_DECODER_H
