/**
 * @file device_test.cpp
 * @brief core/device.h: an AudioDevice on miniaudio's null backend starts and stops cleanly
 *        (no hang, no throw, no sound card needed), and DeviceConfig::from_env() honours
 *        SFX_DEVICE=null -- the switch every headless consumer relies on.
 */
#include <coopa/testing/test.h>

#include <sfxcoopa/core/device.h>
#include <sfxcoopa/core/engine.h>
#include <sfxcoopa/data/audio_format.h>

COOPA_TEST_SUITE("device");

using namespace coopa::sfx::core;

COOPA_TEST(null_backend_device_starts_and_stops) {
    AudioEngine engine(coopa::sfx::data::AudioFormat{48000, 2});
    DeviceConfig config;
    config.null_backend = true;
    bool constructed = false;
    {
        // construct + start in ctor, stop + uninit in dtor: must not throw or hang
        AudioDevice device(config, [&](float* out, uint32_t frames) { engine.render_offline(out, frames); });
        constructed = true;
    }
    EXPECT_TRUE(constructed);
}

COOPA_TEST(config_from_env_reads_sfx_device_null) {
    coopa::test::ScopedEnv env("SFX_DEVICE", "null");
    EXPECT_TRUE(DeviceConfig::from_env().null_backend);
}
