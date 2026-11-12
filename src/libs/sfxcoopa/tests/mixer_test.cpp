/**
 * @file mixer_test.cpp
 * @brief mixer/mixer.h: the audio-thread Mixer's command queue -- voices sum, commands carrying
 *        a stale handle generation are dropped, and every queued command drains before the
 *        block it was queued for renders.
 */
#include <coopa/testing/test.h>

#include <sfxcoopa/data/audio_format.h>
#include <sfxcoopa/mixer/mixer.h>

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
