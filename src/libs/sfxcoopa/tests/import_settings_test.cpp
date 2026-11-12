/**
 * @file import_settings_test.cpp
 * @brief data/import_settings.h and data/wav_chunks.h: the `.import` sidecar (round trip,
 *        missing-file defaults) and the precedence chain sidecar > embedded `smpl` loop >
 *        defaults, including reading `smpl` loops past a leading JUNK chunk.
 */
#include <coopa/testing/test.h>

#include "support/clip_fixtures.h"

#include <sfxcoopa/data/import_settings.h>
#include <sfxcoopa/data/wav_chunks.h>

#include <glm/glm.hpp>

COOPA_TEST_SUITE("import_settings");

using namespace coopa::sfx::data;

COOPA_TEST(smpl_loop_is_read_past_a_leading_junk_chunk) {
    const std::string path = sfx_test::write_synthetic_wav_with_loop(1000, 5000);
    std::vector<SampleLoop> loops = read_smpl_loops(path);
    ASSERT_EQ(loops.size(), static_cast<size_t>(1));
    EXPECT_EQ(loops[0].start, 1000u);
    EXPECT_EQ(loops[0].end, 5000u);
}

COOPA_TEST(missing_sidecar_yields_defaults_without_throwing) {
    const std::string path = (coopa::test::scratch_dir() / "no_such_clip.wav").string();
    ClipImportSettings settings = ClipImportSettings::load(path);
    ClipImportSettings defaults;
    EXPECT_TRUE(settings.load_type == defaults.load_type);
    EXPECT_EQ(settings.force_mono, defaults.force_mono);
    EXPECT_NEAR(settings.volume, defaults.volume, 1e-6f);
    EXPECT_NEAR(settings.pitch, defaults.pitch, 1e-6f);
    EXPECT_EQ(settings.loop, defaults.loop);
    EXPECT_EQ(settings.default_bus, defaults.default_bus);
    EXPECT_EQ(settings.priority, defaults.priority);
    EXPECT_EQ(settings.spatialize, defaults.spatialize);
}

COOPA_TEST(sidecar_round_trips_every_field) {
    const std::string path = (coopa::test::scratch_dir() / "round_trip_clip.wav").string();

    ClipImportSettings original;
    original.load_type = LoadType::Streaming;
    original.force_mono = true;
    original.sample_rate_override = 22050;
    original.volume = 0.5f;
    original.pitch = 1.2f;
    original.loop = true;
    original.loop_start_frame = 100;
    original.loop_end_frame = 200;
    original.default_bus = "SFX";
    original.priority = 5;
    original.spatialize = true;
    original.spatial.position = glm::vec3(1.0f, 2.0f, 3.0f);
    original.spatial.attenuation.min_distance = 2.0f;
    original.spatial.attenuation.max_distance = 50.0f;
    original.spatial.attenuation.mode = coopa::sfx::spatial::RolloffMode::Linear;
    original.spatial.doppler_factor = 0.8f;
    original.spatial.enable_itd = false;

    original.save(path);
    ClipImportSettings loaded = ClipImportSettings::load(path);

    EXPECT_TRUE(loaded.load_type == LoadType::Streaming);
    EXPECT_EQ(loaded.force_mono, true);
    EXPECT_EQ(loaded.sample_rate_override, 22050u);
    EXPECT_NEAR(loaded.volume, 0.5f, 1e-5f);
    EXPECT_NEAR(loaded.pitch, 1.2f, 1e-5f);
    EXPECT_EQ(loaded.loop, true);
    EXPECT_EQ(loaded.loop_start_frame, static_cast<uint64_t>(100));
    EXPECT_EQ(loaded.loop_end_frame, static_cast<uint64_t>(200));
    EXPECT_EQ(loaded.default_bus, std::string("SFX"));
    EXPECT_EQ(loaded.priority, 5);
    EXPECT_EQ(loaded.spatialize, true);
    EXPECT_VEC_NEAR(loaded.spatial.position, original.spatial.position, 1e-5f);
    EXPECT_NEAR(loaded.spatial.attenuation.min_distance, 2.0f, 1e-5f);
    EXPECT_NEAR(loaded.spatial.attenuation.max_distance, 50.0f, 1e-5f);
    EXPECT_TRUE(loaded.spatial.attenuation.mode == coopa::sfx::spatial::RolloffMode::Linear);
    EXPECT_NEAR(loaded.spatial.doppler_factor, 0.8f, 1e-5f);
    EXPECT_EQ(loaded.spatial.enable_itd, false);
}

COOPA_TEST(sidecar_loop_points_win_over_embedded_smpl_loop) {
    // Tier 4 -> 3: a wav with an authored smpl loop and no sidecar picks up
    // the smpl loop points as its defaults.
    const std::string path = sfx_test::write_synthetic_wav_with_loop(1000, 5000, "precedence_clip.wav");
    ClipImportSettings from_smpl = ClipImportSettings::load(path);
    EXPECT_EQ(from_smpl.loop, true);
    EXPECT_EQ(from_smpl.loop_start_frame, static_cast<uint64_t>(1000));
    EXPECT_EQ(from_smpl.loop_end_frame, static_cast<uint64_t>(5000));

    // Tier 3 -> 2: a sidecar's loop points win over the embedded smpl loop.
    // (Tier 1 -- an explicit settings struct -- is a caller bypassing load(), not merge logic.)
    ClipImportSettings sidecar_override;
    sidecar_override.loop = true;
    sidecar_override.loop_start_frame = 7000;
    sidecar_override.loop_end_frame = 8000;
    sidecar_override.save(path);
    ClipImportSettings from_sidecar = ClipImportSettings::load(path);
    EXPECT_EQ(from_sidecar.loop_start_frame, static_cast<uint64_t>(7000));
    EXPECT_EQ(from_sidecar.loop_end_frame, static_cast<uint64_t>(8000));
}
