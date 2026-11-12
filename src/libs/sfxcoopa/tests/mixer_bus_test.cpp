/**
 * @file mixer_bus_test.cpp
 * @brief mixer/bus.h: the bus graph -- mute, nested gains multiplying through render(), effect
 *        chains running in insertion order, and solo propagating through a three-level graph.
 *
 * Not separately covered: a single bus's volume (nested_gains covers it) and sibling solo (the
 * three-level solo test covers the same rule one level deeper).
 */
#include <coopa/testing/test.h>

#include <sfxcoopa/data/audio_format.h>
#include <sfxcoopa/dsp/effect.h>
#include <sfxcoopa/mixer/bus.h>

#include <algorithm>
#include <memory>

COOPA_TEST_SUITE("mixer_bus");

using namespace coopa::sfx::mixer;
using coopa::sfx::data::AudioFormat;

namespace {
class ScaleEffect : public coopa::sfx::dsp::IEffect {
public:
    explicit ScaleEffect(float factor) : factor_(factor) {}
    void prepare(const AudioFormat&, uint32_t) override {}
    void process(float* buffer, uint32_t frames, uint32_t channels) override {
        for (uint32_t i = 0; i < frames * channels; ++i) buffer[i] *= factor_;
    }

private:
    float factor_;
};

class ClampEffect : public coopa::sfx::dsp::IEffect {
public:
    explicit ClampEffect(float max_abs) : max_abs_(max_abs) {}
    void prepare(const AudioFormat&, uint32_t) override {}
    void process(float* buffer, uint32_t frames, uint32_t channels) override {
        for (uint32_t i = 0; i < frames * channels; ++i) buffer[i] = std::clamp(buffer[i], -max_abs_, max_abs_);
    }

private:
    float max_abs_;
};
}  // namespace

COOPA_TEST(muted_bus_renders_silence) {
    AudioFormat fmt{44100, 1};
    MixerBus bus("Test", 0, nullptr);
    bus.prepare(fmt, 16);
    bus.set_mute(true);
    MixerBus::recompute_effective_mutes({&bus});
    bus.clear_scratch(4);
    for (int i = 0; i < 4; ++i) bus.scratch()[i] = 1.0f;
    bus.render(4);
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(bus.scratch()[i], 0.0f, 1e-6f);
}

COOPA_TEST(nested_bus_gains_multiply) {
    AudioFormat fmt{44100, 1};
    MixerBus parent("Parent", 0, nullptr);
    MixerBus child("Child", 1, &parent);
    parent.add_child(&child);
    parent.prepare(fmt, 16);
    child.prepare(fmt, 16);
    parent.set_volume(0.5f);
    child.set_volume(0.5f);

    parent.clear_scratch(4);
    child.clear_scratch(4);
    for (int i = 0; i < 4; ++i) child.scratch()[i] = 1.0f;  // a "voice" writes into the child only
    parent.render(4);  // recursively renders child first, then accumulates + applies its own gain
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(parent.scratch()[i], 0.25f, 1e-6f);  // 0.5 * 0.5
}

COOPA_TEST(effect_chain_applies_in_insertion_order) {
    AudioFormat fmt{44100, 1};

    MixerBus scale_then_clamp("A", 0, nullptr);
    scale_then_clamp.add_effect(std::make_unique<ScaleEffect>(2.0f));
    scale_then_clamp.add_effect(std::make_unique<ClampEffect>(1.0f));
    scale_then_clamp.prepare(fmt, 16);
    scale_then_clamp.clear_scratch(1);
    scale_then_clamp.scratch()[0] = 0.8f;
    scale_then_clamp.render(1);

    MixerBus clamp_then_scale("B", 0, nullptr);
    clamp_then_scale.add_effect(std::make_unique<ClampEffect>(1.0f));
    clamp_then_scale.add_effect(std::make_unique<ScaleEffect>(2.0f));
    clamp_then_scale.prepare(fmt, 16);
    clamp_then_scale.clear_scratch(1);
    clamp_then_scale.scratch()[0] = 0.8f;
    clamp_then_scale.render(1);

    EXPECT_NEAR(scale_then_clamp.scratch()[0], 1.0f, 1e-6f);  // 0.8*2=1.6, clamped to 1.0
    EXPECT_NEAR(clamp_then_scale.scratch()[0], 1.6f, 1e-6f);  // clamp(0.8)=0.8, *2=1.6
}

COOPA_TEST(solo_two_levels_down_silences_unsoloed_branches) {
    AudioFormat fmt{44100, 1};
    MixerBus master("Master", 0, nullptr);
    MixerBus music("Music", 1, &master);
    MixerBus sfx_bus("SFX", 2, &master);
    MixerBus ambience("Ambience", 3, &sfx_bus);  // two levels down from master
    master.add_child(&music);
    master.add_child(&sfx_bus);
    sfx_bus.add_child(&ambience);
    for (MixerBus* b : {&master, &music, &sfx_bus, &ambience}) b->prepare(fmt, 16);

    ambience.set_solo(true);
    MixerBus::recompute_effective_mutes({&master, &music, &sfx_bus, &ambience});

    for (MixerBus* b : {&master, &music, &sfx_bus, &ambience}) b->clear_scratch(4);
    for (int i = 0; i < 4; ++i) {
        music.scratch()[i] = 1.0f;
        ambience.scratch()[i] = 1.0f;
    }
    master.render(4);

    // Only ambience (soloed) should reach master; music (not soloed, and
    // sfx_bus itself carries no direct signal) should be silenced.
    for (int i = 0; i < 4; ++i) EXPECT_NEAR(master.scratch()[i], 1.0f, 1e-6f);
}
