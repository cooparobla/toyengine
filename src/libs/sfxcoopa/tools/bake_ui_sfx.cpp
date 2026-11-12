/**
 * @file bake_ui_sfx.cpp
 * @brief Offline generator for the sample sound-effect library consumed by
 * uicoopa (assets/sounds/). Not a test and not part of the headless suite --
 * a one-shot content-generation tool you run manually and commit the output
 * of, so the shipped .wav files stay reproducible and every preset stays a
 * few tweakable floats in this file rather than an opaque binary blob.
 *
 * Every preset is built from two small primitives -- a swept-oscillator
 * "tone" layer and a filtered-noise "noise" layer, both under an ADSR
 * envelope -- summed together at independent start offsets. That is enough
 * to cover clicks, sweeps, chords/arpeggios (multiple tone layers staggered
 * in time) and noise-based textures (whooshes, hits, explosions) without a
 * bespoke code path per sound.
 *
 * Deliberately does NOT use miniaudio's ma_waveform/ma_noise generators:
 * coopa::sfx's public target declares MA_NO_GENERATION (see
 * ../CMakeLists.txt), and that define must stay identical across every
 * translation unit in the dependency graph or miniaudio's structs change
 * shape -- so this tool's oscillators and noise are hand-rolled instead.
 * Filtering reuses sfxcoopa's own DSP building blocks (dsp/biquad.h,
 * dsp/delay_line.h) rather than reimplementing them.
 *
 * @code
 * cbuild
 * ./build/sfxcoopa_bake /home/coopa/git/uicoopa/assets/sounds
 * @endcode
 */

#include <sfxcoopa/dsp/biquad.h>
#include <sfxcoopa/dsp/delay_line.h>
#include <sfxcoopa/util/error.h>

#include <miniaudio/miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using coopa::sfx::dsp::Biquad;
using coopa::sfx::dsp::DelayLine;

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr float kPi = 3.14159265358979323846f;

// --- Oscillator ------------------------------------------------------------

enum class Waveform { Sine, Square, Saw, Triangle };

/** @brief Evaluates one cycle of `wave` at phase [0, 1). */
float oscillator(Waveform wave, float phase) {
    switch (wave) {
        case Waveform::Sine: return std::sin(2.0f * kPi * phase);
        case Waveform::Square: return phase < 0.5f ? 1.0f : -1.0f;
        case Waveform::Saw: return 2.0f * phase - 1.0f;
        case Waveform::Triangle: return 4.0f * std::abs(phase - 0.5f) - 1.0f;
    }
    return 0.0f;
}

// --- Noise -------------------------------------------------------------

/** @brief Small deterministic PRNG so a preset's noise layer bakes identically every run. */
class Xorshift32 {
public:
    explicit Xorshift32(uint32_t seed) : state_(seed ? seed : 1) {}
    float next_signed() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return (static_cast<float>(state_) / static_cast<float>(UINT32_MAX)) * 2.0f - 1.0f;
    }

private:
    uint32_t state_;
};

// --- Envelope ------------------------------------------------------------

/** @brief Attack/decay/sustain(-hold)/release envelope, evaluated as a function of time. */
struct AdsrSpec {
    float attack = 0.005f;
    float decay = 0.05f;
    float sustain_level = 0.5f;
    float sustain_time = 0.0f;
    float release = 0.05f;
};

float adsr_value(const AdsrSpec& a, float t, float layer_duration) {
    if (t < 0.0f) return 0.0f;
    const float sustain_end = a.attack + a.decay + a.sustain_time;
    const float release_end = sustain_end + a.release;
    if (t < a.attack) {
        return a.attack > 0.0f ? t / a.attack : 1.0f;
    }
    if (t < a.attack + a.decay) {
        const float d = (t - a.attack) / std::max(a.decay, 1e-6f);
        return 1.0f + (a.sustain_level - 1.0f) * d;
    }
    if (t < sustain_end) {
        return a.sustain_level;
    }
    if (t < release_end) {
        const float r = (t - sustain_end) / std::max(a.release, 1e-6f);
        return a.sustain_level * (1.0f - r);
    }
    (void)layer_duration;
    return 0.0f;
}

// --- Layers ------------------------------------------------------------

/** @brief A swept-oscillator voice: frequency glides from start_hz to end_hz over its own duration. */
struct ToneLayer {
    float start_time = 0.0f;
    float duration = 0.12f;
    float start_hz = 440.0f;
    float end_hz = 440.0f;
    bool exponential_sweep = true;
    Waveform wave = Waveform::Sine;
    float level = 1.0f;
    AdsrSpec env{};
};

/** @brief A filtered-noise voice: white noise through an optional lowpass and/or highpass. */
struct NoiseLayer {
    float start_time = 0.0f;
    float duration = 0.15f;
    float lowpass_hz = 0.0f;  // 0 = no lowpass
    float highpass_hz = 0.0f; // 0 = no highpass
    float level = 1.0f;
    uint32_t seed = 1;
    AdsrSpec env{};
};

/** @brief One baked sound effect: any number of tone/noise layers, an optional short echo tail,
 *  and the manifest metadata (category/gain/pitch_jitter) shipped alongside it. */
struct SfxSpec {
    std::string name;
    std::string category;
    float total_duration = 0.25f;
    std::vector<ToneLayer> tones;
    std::vector<NoiseLayer> noises;
    float bitcrush_bits = 0.0f; // 0 = off; else quantize to this many bits (texture, e.g. lasers)
    float echo_seconds = 0.0f; // 0 = off; else a single short delay-line repeat, for whooshes/creaks
    float echo_mix = 0.35f;
    float manifest_gain = 0.85f;
    float pitch_jitter = 0.03f;
};

/** @brief Renders `spec` to mono f32 PCM at kSampleRate, peak-normalized to manifest_gain. */
std::vector<float> render(const SfxSpec& spec) {
    const size_t frame_count = static_cast<size_t>(spec.total_duration * kSampleRate) + 1;
    std::vector<float> out(frame_count, 0.0f);

    for (const ToneLayer& layer : spec.tones) {
        const size_t start = static_cast<size_t>(layer.start_time * kSampleRate);
        const size_t length = static_cast<size_t>(layer.duration * kSampleRate);
        float phase = 0.0f;
        for (size_t i = 0; i < length && start + i < out.size(); ++i) {
            const float t = static_cast<float>(i) / kSampleRate;
            const float frac = layer.duration > 0.0f ? t / layer.duration : 0.0f;
            const float hz = layer.exponential_sweep
                                 ? layer.start_hz * std::pow(layer.end_hz / std::max(layer.start_hz, 1e-3f), frac)
                                 : layer.start_hz + (layer.end_hz - layer.start_hz) * frac;
            phase += hz / kSampleRate;
            phase -= std::floor(phase);
            const float env = adsr_value(layer.env, t, layer.duration);
            out[start + i] += oscillator(layer.wave, phase) * env * layer.level;
        }
    }

    for (const NoiseLayer& layer : spec.noises) {
        const size_t start = static_cast<size_t>(layer.start_time * kSampleRate);
        const size_t length = static_cast<size_t>(layer.duration * kSampleRate);
        Xorshift32 rng(layer.seed);
        Biquad lp = layer.lowpass_hz > 0.0f ? Biquad::lowpass(layer.lowpass_hz, kSampleRate) : Biquad();
        Biquad hp = layer.highpass_hz > 0.0f ? Biquad::highpass(layer.highpass_hz, kSampleRate) : Biquad();
        for (size_t i = 0; i < length && start + i < out.size(); ++i) {
            const float t = static_cast<float>(i) / kSampleRate;
            float sample = rng.next_signed();
            if (layer.lowpass_hz > 0.0f) sample = lp.process(sample);
            if (layer.highpass_hz > 0.0f) sample = hp.process(sample);
            const float env = adsr_value(layer.env, t, layer.duration);
            out[start + i] += sample * env * layer.level;
        }
    }

    if (spec.echo_seconds > 0.0f) {
        DelayLine delay(static_cast<uint32_t>(spec.echo_seconds * kSampleRate) + 1);
        delay.set_delay_samples(spec.echo_seconds * kSampleRate);
        for (float& sample : out) {
            const float tap = delay.read();
            delay.push(sample);
            sample += tap * spec.echo_mix;
        }
    }

    if (spec.bitcrush_bits > 0.0f) {
        const float levels = std::pow(2.0f, spec.bitcrush_bits);
        for (float& sample : out) sample = std::round(sample * levels) / levels;
    }

    // Peak-normalize to manifest_gain (never boosts silence) and apply a 2ms
    // fade in/out so the file's own edges never click.
    float peak = 0.0f;
    for (float sample : out) peak = std::max(peak, std::abs(sample));
    if (peak > 1e-6f) {
        const float scale = spec.manifest_gain / peak;
        for (float& sample : out) sample *= scale;
    }
    const size_t fade_samples = std::min<size_t>(out.size() / 2, kSampleRate / 500); // 2ms
    for (size_t i = 0; i < fade_samples; ++i) {
        const float g = static_cast<float>(i) / static_cast<float>(fade_samples);
        out[i] *= g;
        out[out.size() - 1 - i] *= g;
    }

    return out;
}

// --- Preset factories ------------------------------------------------------

SfxSpec make_click(std::string name, float hz, float duration, Waveform wave, float gain) {
    SfxSpec s;
    s.name = std::move(name);
    s.category = "ui";
    s.total_duration = duration + 0.02f;
    s.manifest_gain = gain;
    s.pitch_jitter = 0.03f;
    ToneLayer tone;
    tone.duration = duration;
    tone.start_hz = hz;
    tone.end_hz = hz * 0.85f;
    tone.wave = wave;
    tone.env = AdsrSpec{.attack = 0.001f, .decay = duration * 0.6f, .sustain_level = 0.0f, .sustain_time = 0.0f,
                         .release = duration * 0.3f};
    s.tones.push_back(tone);
    return s;
}

SfxSpec make_hover(std::string name, float hz, float duration) {
    SfxSpec s;
    s.name = std::move(name);
    s.category = "ui";
    s.total_duration = duration + 0.02f;
    s.manifest_gain = 0.5f;
    s.pitch_jitter = 0.04f;
    ToneLayer tone;
    tone.duration = duration;
    tone.start_hz = hz;
    tone.end_hz = hz * 1.1f;
    tone.wave = Waveform::Sine;
    tone.env = AdsrSpec{.attack = 0.01f, .decay = duration * 0.5f, .sustain_level = 0.0f, .sustain_time = 0.0f,
                         .release = duration * 0.4f};
    s.tones.push_back(tone);
    return s;
}

SfxSpec make_toggle(std::string name, bool turning_on) {
    SfxSpec s;
    s.name = std::move(name);
    s.category = "ui";
    s.total_duration = 0.14f;
    s.manifest_gain = 0.75f;
    s.pitch_jitter = 0.02f;
    ToneLayer a, b;
    a.duration = 0.05f;
    b.start_time = 0.045f;
    b.duration = 0.06f;
    const float lo = 520.0f, hi = 780.0f;
    a.start_hz = a.end_hz = turning_on ? lo : hi;
    b.start_hz = b.end_hz = turning_on ? hi : lo;
    a.wave = b.wave = Waveform::Triangle;
    a.env = b.env = AdsrSpec{.attack = 0.002f, .decay = 0.03f, .sustain_level = 0.0f, .sustain_time = 0.0f,
                              .release = 0.02f};
    s.tones = {a, b};
    return s;
}

SfxSpec make_sweep(std::string name, std::string category, float start_hz, float end_hz, float duration,
                    Waveform wave, float noise_mix, float gain) {
    SfxSpec s;
    s.name = std::move(name);
    s.category = std::move(category);
    s.total_duration = duration + 0.03f;
    s.manifest_gain = gain;
    s.pitch_jitter = 0.02f;
    ToneLayer tone;
    tone.duration = duration;
    tone.start_hz = start_hz;
    tone.end_hz = end_hz;
    tone.wave = wave;
    tone.env = AdsrSpec{.attack = duration * 0.1f, .decay = duration * 0.3f, .sustain_level = 0.4f,
                         .sustain_time = duration * 0.2f, .release = duration * 0.4f};
    s.tones.push_back(tone);
    if (noise_mix > 0.0f) {
        NoiseLayer noise;
        noise.duration = duration;
        noise.lowpass_hz = std::max(start_hz, end_hz) * 1.5f;
        noise.highpass_hz = std::min(start_hz, end_hz) * 0.5f;
        noise.level = noise_mix;
        noise.env = tone.env;
        s.noises.push_back(noise);
    }
    return s;
}

SfxSpec make_noise_burst(std::string name, std::string category, float duration, float lowpass_hz,
                          float highpass_hz, float gain) {
    SfxSpec s;
    s.name = std::move(name);
    s.category = std::move(category);
    s.total_duration = duration + 0.03f;
    s.manifest_gain = gain;
    s.pitch_jitter = 0.0f;
    NoiseLayer noise;
    noise.duration = duration;
    noise.lowpass_hz = lowpass_hz;
    noise.highpass_hz = highpass_hz;
    noise.env = AdsrSpec{.attack = 0.002f, .decay = duration * 0.4f, .sustain_level = 0.2f,
                          .sustain_time = duration * 0.1f, .release = duration * 0.5f};
    s.noises.push_back(noise);
    return s;
}

/** @brief A short melodic arpeggio -- N tone layers, each a brief blip, staggered in time. Used
 *  for confirm/success/complete/level_up/coin/notify/bell/chime, which all read as "a few
 *  ascending (or descending) notes" rather than a single tone. */
SfxSpec make_arpeggio(std::string name, std::string category, std::vector<float> freqs_hz, float note_duration,
                       float note_gap, Waveform wave, float gain) {
    SfxSpec s;
    s.name = std::move(name);
    s.category = std::move(category);
    s.manifest_gain = gain;
    s.pitch_jitter = 0.015f;
    float t = 0.0f;
    for (float hz : freqs_hz) {
        ToneLayer tone;
        tone.start_time = t;
        tone.duration = note_duration;
        tone.start_hz = tone.end_hz = hz;
        tone.wave = wave;
        tone.env = AdsrSpec{.attack = 0.004f, .decay = note_duration * 0.3f, .sustain_level = 0.5f,
                             .sustain_time = note_duration * 0.2f, .release = note_duration * 0.5f};
        s.tones.push_back(tone);
        t += note_gap;
    }
    s.total_duration = t + note_duration + 0.05f;
    return s;
}

/** @brief Builds the full ~40-sound preset table described in the uicoopa audio integration plan. */
std::vector<SfxSpec> build_presets() {
    std::vector<SfxSpec> presets;

    // --- ui (14) ---
    presets.push_back(make_click("ui_click_soft", 720.0f, 0.05f, Waveform::Sine, 0.6f));
    presets.push_back(make_click("ui_click_sharp", 1400.0f, 0.03f, Waveform::Square, 0.5f));
    presets.push_back(make_click("ui_click_deep", 260.0f, 0.07f, Waveform::Triangle, 0.7f));
    presets.push_back(make_hover("ui_hover_tick", 1600.0f, 0.03f));
    presets.push_back(make_hover("ui_hover_soft", 900.0f, 0.05f));
    presets.push_back(make_toggle("ui_toggle_on", true));
    presets.push_back(make_toggle("ui_toggle_off", false));
    presets.push_back(make_click("ui_slider_tick", 1100.0f, 0.02f, Waveform::Square, 0.35f));
    presets.push_back(make_sweep("ui_panel_open", "ui", 300.0f, 900.0f, 0.18f, Waveform::Triangle, 0.1f, 0.6f));
    presets.push_back(make_sweep("ui_panel_close", "ui", 900.0f, 300.0f, 0.16f, Waveform::Triangle, 0.1f, 0.6f));
    presets.push_back(make_hover("ui_focus", 1200.0f, 0.04f));
    presets.push_back(make_arpeggio("ui_tab_switch", "ui", {700.0f, 1000.0f}, 0.035f, 0.03f, Waveform::Sine, 0.5f));
    presets.push_back(make_click("ui_type_key", 2200.0f, 0.012f, Waveform::Square, 0.3f));
    presets.push_back(make_click("ui_scroll_notch", 500.0f, 0.02f, Waveform::Square, 0.35f));

    // --- feedback (8) ---
    presets.push_back(make_arpeggio("confirm", "feedback", {660.0f, 990.0f}, 0.06f, 0.05f, Waveform::Sine, 0.7f));
    presets.push_back(
        make_arpeggio("success", "feedback", {523.25f, 659.25f, 783.99f}, 0.08f, 0.07f, Waveform::Sine, 0.75f));
    presets.push_back(make_sweep("error", "feedback", 400.0f, 150.0f, 0.22f, Waveform::Square, 0.25f, 0.7f));
    presets.push_back(make_sweep("warning", "feedback", 500.0f, 350.0f, 0.14f, Waveform::Square, 0.15f, 0.6f));
    presets.push_back(make_sweep("denied", "feedback", 300.0f, 180.0f, 0.12f, Waveform::Saw, 0.2f, 0.6f));
    presets.push_back(make_arpeggio("notify", "feedback", {880.0f, 1174.66f}, 0.05f, 0.06f, Waveform::Sine, 0.55f));
    presets.push_back(make_arpeggio("complete", "feedback", {523.25f, 659.25f, 783.99f, 1046.5f}, 0.07f, 0.06f,
                                     Waveform::Triangle, 0.75f));
    presets.push_back(
        make_arpeggio("level_up", "feedback", {523.25f, 659.25f, 783.99f, 1046.5f, 1318.5f}, 0.06f, 0.05f,
                       Waveform::Square, 0.75f));

    // --- transition (5) ---
    {
        SfxSpec whoosh = make_sweep("whoosh_short", "transition", 200.0f, 2000.0f, 0.25f, Waveform::Sine, 0.6f, 0.55f);
        presets.push_back(whoosh);
    }
    {
        SfxSpec whoosh_long = make_sweep("whoosh_long", "transition", 120.0f, 1600.0f, 0.7f, Waveform::Sine, 0.65f, 0.55f);
        whoosh_long.echo_seconds = 0.09f;
        whoosh_long.echo_mix = 0.25f;
        presets.push_back(whoosh_long);
    }
    presets.push_back(make_sweep("swipe", "transition", 1800.0f, 400.0f, 0.2f, Waveform::Sine, 0.5f, 0.5f));
    presets.push_back(make_click("pop", 950.0f, 0.04f, Waveform::Sine, 0.65f));
    {
        SfxSpec page = make_noise_burst("page_turn", "transition", 0.22f, 3500.0f, 800.0f, 0.4f);
        page.echo_seconds = 0.05f;
        page.echo_mix = 0.2f;
        presets.push_back(page);
    }

    // --- game (9) ---
    presets.push_back(
        make_arpeggio("coin", "game", {988.0f, 1318.5f}, 0.05f, 0.045f, Waveform::Square, 0.7f));
    presets.push_back(make_sweep("jump", "game", 300.0f, 700.0f, 0.14f, Waveform::Square, 0.0f, 0.6f));
    {
        SfxSpec laser = make_sweep("laser", "game", 1800.0f, 200.0f, 0.18f, Waveform::Saw, 0.15f, 0.65f);
        laser.bitcrush_bits = 6.0f;
        presets.push_back(laser);
    }
    {
        SfxSpec explosion = make_noise_burst("explosion", "game", 0.6f, 1200.0f, 0.0f, 0.85f);
        ToneLayer thump;
        thump.duration = 0.3f;
        thump.start_hz = 90.0f;
        thump.end_hz = 40.0f;
        thump.wave = Waveform::Sine;
        thump.env = AdsrSpec{.attack = 0.002f, .decay = 0.12f, .sustain_level = 0.3f, .sustain_time = 0.05f,
                              .release = 0.15f};
        thump.level = 0.8f;
        explosion.tones.push_back(thump);
        presets.push_back(explosion);
    }
    presets.push_back(make_noise_burst("hit", "game", 0.1f, 2500.0f, 300.0f, 0.7f));
    presets.push_back(
        make_arpeggio("powerup", "game", {523.25f, 659.25f, 783.99f, 1046.5f}, 0.045f, 0.04f, Waveform::Square, 0.7f));
    presets.push_back(make_click("pickup", 1500.0f, 0.05f, Waveform::Sine, 0.6f));
    presets.push_back(make_noise_burst("footstep", "game", 0.06f, 900.0f, 0.0f, 0.4f));
    {
        SfxSpec creak = make_noise_burst("door_creak", "game", 0.5f, 1800.0f, 200.0f, 0.5f);
        ToneLayer groan;
        groan.duration = 0.45f;
        groan.start_hz = 180.0f;
        groan.end_hz = 260.0f;
        groan.exponential_sweep = false;
        groan.wave = Waveform::Saw;
        groan.level = 0.3f;
        groan.env = AdsrSpec{.attack = 0.05f, .decay = 0.1f, .sustain_level = 0.5f, .sustain_time = 0.2f,
                              .release = 0.1f};
        creak.tones.push_back(groan);
        presets.push_back(creak);
    }

    // --- tonal (4) ---
    presets.push_back(make_arpeggio("chime", "tonal", {1046.5f, 1318.5f, 1568.0f}, 0.12f, 0.1f, Waveform::Sine, 0.7f));
    presets.push_back(make_click("bell", 987.77f, 0.4f, Waveform::Sine, 0.65f));
    presets.push_back(make_sweep("blip_up", "tonal", 440.0f, 880.0f, 0.08f, Waveform::Square, 0.0f, 0.55f));
    presets.push_back(make_sweep("blip_down", "tonal", 880.0f, 440.0f, 0.08f, Waveform::Square, 0.0f, 0.55f));

    return presets;
}

/** @brief Writes `samples` (mono f32, kSampleRate) to `path` as 48kHz/mono/16-bit PCM WAV.
 *  ma_encoder performs no format conversion (see miniaudio's own docs), so the f32 -> s16
 *  conversion with clamping happens here before ma_encoder_write_pcm_frames(). */
void write_wav(const std::string& path, const std::vector<float>& samples) {
    ma_encoder_config config = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, kSampleRate);
    ma_encoder encoder;
    SFX_MA_CHECK(ma_encoder_init_file(path.c_str(), &config, &encoder));

    std::vector<int16_t> pcm(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        const float clamped = std::clamp(samples[i], -1.0f, 1.0f);
        pcm[i] = static_cast<int16_t>(std::lround(clamped * 32767.0f));
    }

    ma_uint64 written = 0;
    SFX_MA_CHECK(ma_encoder_write_pcm_frames(&encoder, pcm.data(), pcm.size(), &written));
    ma_encoder_uninit(&encoder);
}

/** @brief Writes the sounds.yaml manifest uicoopa's SoundLibrary loads (see
 *  uicoopa/audio/sound_library.h). Hand-written rather than via fkYAML/YAMLMap
 *  since this tool is standalone and the shape is trivial. */
void write_manifest(const std::string& dir, const std::vector<SfxSpec>& presets) {
    std::ofstream out(dir + "/sounds.yaml");
    out << "# Generated by sfxcoopa/tools/bake_ui_sfx.cpp -- do not hand-edit; edit the\n"
        << "# generator and re-run instead so the sounds and manifest stay in sync.\n"
        << "sounds:\n";
    for (const SfxSpec& s : presets) {
        out << "  - name: " << s.name << "\n"
            << "    file: " << s.name << ".wav\n"
            << "    category: " << s.category << "\n"
            << "    gain: " << s.manifest_gain << "\n"
            << "    pitch_jitter: " << s.pitch_jitter << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: sfxcoopa_bake <output_dir>\n";
        return 1;
    }
    const std::string out_dir = argv[1];
    std::filesystem::create_directories(out_dir);

    std::vector<SfxSpec> presets = build_presets();
    for (const SfxSpec& spec : presets) {
        std::vector<float> samples = render(spec);
        write_wav(out_dir + "/" + spec.name + ".wav", samples);
        std::cout << "wrote " << spec.name << ".wav (" << samples.size() << " frames)\n";
    }
    write_manifest(out_dir, presets);
    std::cout << "Baked " << presets.size() << " sound effects to " << out_dir << "\n";
    return 0;
}
