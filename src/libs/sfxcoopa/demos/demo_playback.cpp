/**
 * @file demo_playback.cpp
 * @brief Audible/offline-bounce demo for sfxcoopa. Not a test and not part of
 * the headless suites (tests/) -- built as its own target (sfxcoopa_playback)
 * so ctest/CI never needs a sound card.
 *
 * Creates Music and SFX buses under master at different levels, plays
 * ex/SEFE_Flashback.wav on Music (fully decoded; there is no streaming
 * path), and orbits ex/SEFE_Whoosh03.wav (force-mono, one-shot)
 * around the listener on SFX so ITD/ILD is audible on headphones. Writes
 * `.import` sidecars each run (force_mono + spatialize for the orbiting
 * clip, the Music bus for the other) so the demo is self-contained rather than depending on a
 * checked-in sidecar next to a committed fixture.
 *
 * Env-gated:
 *   MAX_SECONDS=<n>  Auto-exit after n seconds (default 10).
 *   ONESHOT=1        Exit after one device period.
 *   SFX_DEVICE=null  Use miniaudio's null backend (silent, CI-safe).
 *   BOUNCE=<file>    Skip the device entirely; render straight to
 *                    output/<file> via ma_encoder instead of playing it.
 *
 * @code
 * cbuild
 * MAX_SECONDS=10 ./build/sfxcoopa_playback          # audible orbit test
 * BOUNCE=orbit.wav ./build/sfxcoopa_playback        # offline bounce for inspection
 * @endcode
 */

#include <sfxcoopa/core/engine.h>
#include <sfxcoopa/core/device.h>
#include <sfxcoopa/data/import_settings.h>
#include <sfxcoopa/util/error.h>

#include <coopa/util/file.h>

#include <miniaudio/miniaudio.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace coopa::sfx;

namespace {

/** @brief Full orbit period, in seconds, for the SFX source's sweep around the listener. */
constexpr float kOrbitPeriodSeconds = 4.0f;

glm::vec3 orbit_position(float elapsed_seconds) {
    const float angle = elapsed_seconds * (2.0f * 3.14159265358979323846f / kOrbitPeriodSeconds);
    const float radius = 5.0f;
    return glm::vec3(radius * std::sin(angle), 0.0f, -radius * std::cos(angle));
}

/**
 * @brief Removes the .import sidecars this demo writes, on every exit path
 * (including early returns). ex/ is shared, committed fixture data also
 * read by anything else that decodes these clips through the sidecar-aware
 * AudioEngine::load_clip(path) -- a sidecar left behind by one run would
 * otherwise silently change how a *later, unrelated* run decodes the same
 * fixture. (The headless suites copy the fixtures into a per-test scratch
 * directory first, so they are immune either way.)
 */
struct SidecarCleanup {
    std::vector<std::string> paths;
    ~SidecarCleanup() {
        for (const auto& p : paths) std::filesystem::remove(p + ".import");
    }
};

}  // namespace

int main() {
    const uint32_t sample_rate = 48000;
    core::AudioEngine engine(data::AudioFormat{sample_rate, 2});

    core::MixerBus& music = engine.create_bus("Music", engine.master());
    core::MixerBus& sfx_bus = engine.create_bus("SFX", engine.master());
    music.set_volume_db(-6.0f);
    sfx_bus.set_volume_db(0.0f);

    const std::string whoosh_path = FileUtil::get_root_path("ex/SEFE_Whoosh03.wav");
    const std::string flashback_path = FileUtil::get_root_path("ex/SEFE_Flashback.wav");
    SidecarCleanup sidecar_cleanup{{whoosh_path, flashback_path}};

    // The orbiting source must be mono to be spatialized -- write sidecars
    // each run so the demo is self-contained rather than depending on
    // checked-in .import files next to the committed fixtures.
    {
        data::ClipImportSettings whoosh_settings;
        whoosh_settings.force_mono = true;
        whoosh_settings.spatialize = true;
        whoosh_settings.default_bus = "SFX";
        whoosh_settings.save(whoosh_path);

        data::ClipImportSettings flashback_settings;
        flashback_settings.default_bus = "Music";
        flashback_settings.save(flashback_path);
    }

    engine.play(flashback_path);

    spatial::Listener listener;  // origin, facing -Z: matches BinauralPanner's convention
    engine.set_listener(listener);

    core::VoiceHandle orbit_handle = engine.play_3d(whoosh_path, orbit_position(0.0f));

    const bool oneshot = std::getenv("ONESHOT") != nullptr;
    const char* max_seconds_env = std::getenv("MAX_SECONDS");
    const float max_seconds = max_seconds_env ? static_cast<float>(std::atof(max_seconds_env)) : 10.0f;
    const char* bounce_file = std::getenv("BOUNCE");

    if (bounce_file) {
        ma_encoder_config enc_config = ma_encoder_config_init(ma_encoding_format_wav, ma_format_f32, 2, sample_rate);
        ma_encoder encoder;
        const std::string out_dir = FileUtil::get_root_path("output");
        std::filesystem::create_directories(out_dir);
        const std::string full_path = out_dir + "/" + bounce_file;
        SFX_MA_CHECK(ma_encoder_init_file(full_path.c_str(), &enc_config, &encoder));

        const uint64_t total_frames = static_cast<uint64_t>(max_seconds * sample_rate);
        const uint32_t block = 480;
        std::vector<float> buffer(static_cast<size_t>(block) * 2);
        uint64_t rendered = 0;
        while (rendered < total_frames) {
            const uint32_t this_block = static_cast<uint32_t>(std::min<uint64_t>(block, total_frames - rendered));
            const float t = static_cast<float>(rendered) / static_cast<float>(sample_rate);
            engine.set_voice_position(orbit_handle, orbit_position(t));
            engine.update(static_cast<float>(this_block) / static_cast<float>(sample_rate));
            engine.render_offline(buffer.data(), this_block);
            ma_uint64 written = 0;
            ma_encoder_write_pcm_frames(&encoder, buffer.data(), this_block, &written);
            rendered += this_block;
        }
        ma_encoder_uninit(&encoder);
        std::cout << "Bounced " << total_frames << " frames to " << full_path << std::endl;
        return 0;
    }

    core::DeviceConfig device_config = core::DeviceConfig::from_env();
    device_config.sample_rate = sample_rate;
    core::AudioDevice device(device_config,
                              [&](float* out, uint32_t frames) { engine.render_offline(out, frames); });

    if (oneshot) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } else {
        const auto start = std::chrono::steady_clock::now();
        while (true) {
            const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
            if (t >= max_seconds) break;
            engine.set_voice_position(orbit_handle, orbit_position(t));
            engine.update(0.016f);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    return 0;
}
