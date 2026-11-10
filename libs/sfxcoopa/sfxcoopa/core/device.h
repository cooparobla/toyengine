/**
 * @file device.h
 * @brief AudioDevice: a thin RAII shell over ma_device that forwards the
 * hardware callback straight to whatever render function it's given --
 * typically AudioEngine::render_offline(), so the device adds nothing to
 * the signal path beyond opening the actual sound card.
 */

#ifndef SFXCOOPA_CORE_DEVICE_H
#define SFXCOOPA_CORE_DEVICE_H

#include <sfxcoopa/data/audio_format.h>
#include <sfxcoopa/util/error.h>

#include <miniaudio/miniaudio.h>

#include <cstdlib>
#include <cstring>
#include <functional>

namespace coopa {
namespace sfx {
namespace core {

/**
 * @struct DeviceConfig
 * @brief What sound card to open and at what rate/buffer size.
 */
struct DeviceConfig {
    uint32_t sample_rate = coopa::sfx::data::k_default_sample_rate;
    uint32_t buffer_frames = 480;
    bool null_backend = false; /**< Also settable via the SFX_DEVICE=null environment variable; see from_env(). */

    /**
     * @brief Overlays the SFX_DEVICE=null environment variable (the
     * convention this library's headless-safe demo target uses) onto `base`.
     */
    static DeviceConfig from_env(DeviceConfig base) {
        const char* device_env = std::getenv("SFX_DEVICE");
        if (device_env && std::strcmp(device_env, "null") == 0) base.null_backend = true;
        return base;
    }

    /** @brief Overload of from_env() defaulting `base` to DeviceConfig{}. */
    static DeviceConfig from_env() { return from_env(DeviceConfig{}); }
};

/**
 * @class AudioDevice
 * @brief RAII over ma_context + ma_device: opens the device (or the null
 * backend, for CI/headless boxes) in the constructor, starts it
 * immediately, and tears both down in the destructor. Non-copyable,
 * non-movable.
 *
 * @code
 * AudioEngine engine(AudioFormat{48000, 2});
 * AudioDevice device(DeviceConfig::from_env(), [&](float* out, uint32_t frames) {
 *     engine.render_offline(out, frames);
 * });
 * @endcode
 */
class AudioDevice {
public:
    using RenderCallback = std::function<void(float* out, uint32_t frames)>;

    AudioDevice(const DeviceConfig& config, RenderCallback callback) : callback_(std::move(callback)) {
        ma_context_config context_config = ma_context_config_init();
        ma_backend null_backend_list[1] = {ma_backend_null};
        if (config.null_backend) {
            SFX_MA_CHECK(ma_context_init(null_backend_list, 1, &context_config, &context_));
        } else {
            SFX_MA_CHECK(ma_context_init(nullptr, 0, &context_config, &context_));
        }

        ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
        device_config.playback.format = ma_format_f32;
        device_config.playback.channels = 2;
        device_config.sampleRate = config.sample_rate;
        device_config.periodSizeInFrames = config.buffer_frames;
        device_config.dataCallback = &AudioDevice::data_callback_;
        device_config.pUserData = this;

        SFX_MA_CHECK(ma_device_init(&context_, &device_config, &device_));
        device_initialized_ = true;
        SFX_MA_CHECK(ma_device_start(&device_));
    }

    ~AudioDevice() {
        if (device_initialized_) {
            ma_device_uninit(&device_);
            ma_context_uninit(&context_);
        }
    }

    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;
    AudioDevice(AudioDevice&&) = delete;
    AudioDevice& operator=(AudioDevice&&) = delete;

private:
    static void data_callback_(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
        (void)input;
        auto* self = static_cast<AudioDevice*>(device->pUserData);
        self->callback_(static_cast<float*>(output), frame_count);
    }

    ma_context context_{};
    ma_device device_{};
    RenderCallback callback_;
    bool device_initialized_ = false;
};

}  // namespace core
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_CORE_DEVICE_H
