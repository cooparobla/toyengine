/**
 * @file mixer_test.cpp
 * @brief mixer/mixer.h: the audio-thread Mixer's command queue -- voices sum, commands carrying
 *        a stale handle generation are dropped, and every queued command drains before the
 *        block it was queued for renders. Also covers bus publication: create_bus() hands back
 *        buses at stable addresses up to max_buses and then throws, rather than growing storage
 *        that render() is concurrently walking.
 */
#include <coopa/testing/test.h>

#include <sfxcoopa/data/audio_format.h>
#include <sfxcoopa/mixer/mixer.h>

#include <stdexcept>
#include <string>
#include <vector>

COOPA_TEST_SUITE("mixer");

using namespace coopa::sfx::mixer;
using coopa::sfx::data::AudioFormat;

namespace {
Command play_command(VoiceHandle voice, const std::vector<float>& clip) {
    Command play{};
    play.type = CommandType::Play;
    play.voice = voice;
    play.clip_data = clip.data();
    play.clip_frame_count = clip.size();
    play.clip_channels = 1;
    play.gain = 1.0f;
    play.pitch = 1.0f;
    return play;
}
}  // namespace

COOPA_TEST(two_voices_sum) {
    Mixer mixer(AudioFormat{48000, 1}, 4);
    mixer.prepare(64);
    VoicePool& pool = mixer.voice_pool();
    std::vector<float> clip(64, 0.5f);
    mixer.commands().push(play_command(pool.allocate(0, 0), clip));
    mixer.commands().push(play_command(pool.allocate(0, 1), clip));

    std::vector<float> out(64, 0.0f);
    mixer.render(out.data(), 64);
    for (float sample : out) EXPECT_NEAR(sample, 1.0f, 1e-4f);  // 0.5 + 0.5, gain already at target from play()
}

COOPA_TEST(command_with_stale_generation_is_dropped) {
    Mixer mixer(AudioFormat{48000, 1}, 1);  // capacity 1: forces a steal on the 2nd allocate
    mixer.prepare(64);
    VoicePool& pool = mixer.voice_pool();

    VoiceHandle h1 = pool.allocate(0, 0);
    std::vector<float> clip1(64, 0.5f);
    mixer.commands().push(play_command(h1, clip1));
    std::vector<float> out(64, 0.0f);
    mixer.render(out.data(), 64);  // h1 now playing

    VoiceHandle h2 = pool.allocate(0, 1);  // steals h1's slot: same index, bumped generation
    ASSERT_EQ(h2.index, h1.index);
    ASSERT_NE(h2.generation, h1.generation);

    std::vector<float> clip2(64, 0.2f);
    mixer.commands().push(play_command(h2, clip2));

    // Stale: references h1's old generation, which play2 (processed first,
    // same FIFO order as pushed) has already superseded on this slot.
    Command stale_gain{};
    stale_gain.type = CommandType::SetVoiceGain;
    stale_gain.voice = h1;
    stale_gain.gain = 0.0f;
    mixer.commands().push(stale_gain);

    mixer.render(out.data(), 64);
    for (float sample : out) EXPECT_NEAR(sample, 0.2f, 1e-3f);  // clip2's level, unaffected by the stale zero-gain
}

COOPA_TEST(commands_drain_before_the_same_block_renders) {
    Mixer mixer(AudioFormat{48000, 1}, 2);
    mixer.prepare(64);
    VoiceHandle h = mixer.voice_pool().allocate(0, 0);

    std::vector<float> clip(64, 1.0f);
    Command set_gain{};
    set_gain.type = CommandType::SetVoiceGain;
    set_gain.voice = h;
    set_gain.gain = 0.5f;

    mixer.commands().push(play_command(h, clip));
    mixer.commands().push(set_gain);

    std::vector<float> out(64, 0.0f);
    mixer.render(out.data(), 64);
    // Both commands drain before any voice renders, so the gain-target
    // change is already in effect for this very first render() call --
    // one exponential ramp step from 1.0 toward 0.5, not the untouched 1.0
    // that would result if SetVoiceGain only took effect on the *next* call.
    EXPECT_NEAR(out[0], 1.0f + (0.5f - 1.0f) * 0.3f, 1e-4f);
    EXPECT_LT(out[0], 1.0f - 1e-4f);
}

COOPA_TEST(created_buses_are_published_at_stable_addresses) {
    Mixer mixer(AudioFormat{48000, 1}, 4, 256, 256, /*max_buses=*/8);
    mixer.prepare(64);

    EXPECT_EQ(mixer.bus_count(), 1u);  // "Master", created by the constructor
    const MixerBus* const master_before = &mixer.master();

    for (uint32_t i = 1; i < 8; ++i) mixer.create_bus("Bus" + std::to_string(i), &mixer.master());
    EXPECT_EQ(mixer.bus_count(), 8u);

    // Filling the mixer moved nothing: every bus the audio thread had already seen is
    // still where it was, which is the whole point of reserving the storage up front.
    EXPECT_TRUE(master_before == &mixer.master());
    EXPECT_TRUE(mixer.bus_at(0) == &mixer.master());
    EXPECT_EQ(mixer.bus_at(7)->name(), std::string("Bus7"));
    EXPECT_TRUE(mixer.bus_at(8) == nullptr);  // past the end, not out of bounds
}

COOPA_TEST(creating_a_bus_past_max_buses_throws) {
    Mixer mixer(AudioFormat{48000, 1}, 4, 256, 256, /*max_buses=*/2);
    mixer.prepare(64);
    mixer.create_bus("Music", &mixer.master());

    bool threw = false;
    try {
        mixer.create_bus("SFX", &mixer.master());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(mixer.bus_count(), 2u);  // the failed create left the graph untouched
}

COOPA_TEST(a_bus_created_after_prepare_carries_audio) {
    Mixer mixer(AudioFormat{48000, 1}, 4);
    mixer.prepare(64);  // prepared first, so a bus created now has to prepare itself

    MixerBus& late = mixer.create_bus("Late", &mixer.master());
    ASSERT_TRUE(mixer.bus_at(1) == &late);
    late.set_volume(0.5f);
    mixer.update_bus_mutes();

    std::vector<float> clip(64, 1.0f);
    Command play = play_command(mixer.voice_pool().allocate(0, 0), clip);
    play.bus_index = late.index();
    mixer.commands().push(play);

    std::vector<float> out(64, 0.0f);
    mixer.render(out.data(), 64);

    // The late bus has a scratch buffer, is linked under Master, and applies its own
    // gain -- none of which holds if create_bus() skipped prepare() or the link.
    for (float sample : out) EXPECT_NEAR(sample, 0.5f, 1e-4f);
}
