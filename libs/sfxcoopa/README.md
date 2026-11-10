# sfxcoopa

**A header-mostly C++20 game audio library with a bus mixer and binaural 3D sound.**

sfxcoopa decodes WAV and MP3 files, plays many sounds at once through a tree of mixer buses,
and places mono sources in 3D with an analytic binaural panner. It uses
[miniaudio](https://miniaud.io) only to open the sound card and decode files. The mixer, voice
pool, resampler and all 3D math are its own. The whole engine can render offline, with no sound
card, so every feature is testable in CI.

## Features

### Playback and clips
- **WAV and MP3 decoding.** Any source rate and sample format is converted to f32 PCM at the
  engine rate. Clips are fully decoded into memory when loaded.
- **Clip cache.** `AudioEngine::load_clip()` caches clips by path and decode settings. A voice
  keeps its clip alive until it finishes.
- **`.import` sidecars.** `ClipImportSettings` reads and writes a flat YAML file next to the clip
  (`whoosh.wav.import`). It covers mono downmix, a sample-rate override, looping, the default bus
  and spatial defaults.
- **Embedded loop points.** Loop markers in a WAV file's `smpl` chunk are used when no sidecar
  overrides them. The RIFF reader skips `JUNK` and other leading chunks.

### Mixing
- **Voice pool.** 128 voices by default. When the pool is full, a new sound steals the
  lowest-priority, oldest voice.
- **Generational handles.** `VoiceHandle` carries a generation, so a stale handle can never
  change a slot that has been reused.
- **Click-free control.** Gain and pitch ramp in 64-frame control blocks. `stop()` fades out
  over about 5 ms instead of cutting. 3D panning targets are recomputed once per `update()`.
- **Bus graph.** Named buses under `Master`, each with volume (linear or dB), mute, solo and an
  effect chain. Solo keeps the soloed bus, its parents and its children audible.
- **Effect chain.** Implement `dsp::IEffect` and add it to any bus with `add_effect()`.
- **Events.** `on_voice_finished` and `on_voice_looped` signals fire on the main thread from
  `update()`.

### 3D audio
- **Binaural panner.** Woodworth interaural time delay, per-ear head-shadow lowpass and level
  difference. No HRTF data files and no FFT.
- **Distance and cones.** Inverse, linear, logarithmic or custom rolloff curves and
  directional cones. Doppler shifts a spatialized voice's pitch from source and listener
  velocity (scaled by `doppler_factor`).
- **Listener.** Position, velocity and orientation, or set it straight from a libcoopa
  `Transform` with `Listener::set_from_transform()`.

### Scene components
- **`AudioSource`** (`<sfxcoopa/components/audio_source.h>`). Plays a clip on a named bus
  (Master if the bus doesn't exist), with volume, pitch, loop, priority, `play_on_start`, and
  optional binaural spatialization at the owner's world position (velocity from frame-to-frame
  motion). Applies the clip sidecar's `volume`/`pitch` multiplicatively. `play()`, `stop()`,
  `pause()`, `resume()`, `is_playing()`, and an `on_finished` signal. Destroying the component
  stops its voice. A stereo clip asked to spatialize plays in 2D with a one-time warning.
- **`AudioListener`** (`<sfxcoopa/components/audio_listener.h>`). Pushes its owner's world
  transform into the engine listener every update. `apply_listener_matrix(engine, mat)` lets a
  host drive the listener from a camera instead.
- **YAML.** `<sfxcoopa/sfx_yaml.h>` has `register_sfx_components()` (`type: AudioSource` /
  `type: AudioListener`) and the `SfxResources` singleton: the engine pointer, asset search
  roots for relative clip paths, and `consume_listener_pushed()` so a host knows whether an
  `AudioListener` drove the listener this frame. Call `set_engine()` before loading a scene.
  `play_on_start` fires on the source's first `update()`, not during the load, so a scene an
  editor loads only to edit (never simulated) stays silent.

### DSP building blocks
- RBJ biquads (lowpass, highpass, peaking, low and high shelf), a one-pole lowpass, a
  fractional delay line and a cubic-Hermite resampler.
- A lock-free single-producer, single-consumer `RingBuffer` carries commands to the audio
  thread and events back.

## Getting started

### 1. Clone

sfxcoopa builds on [libcoopa](../libcoopa), which must sit in a sibling folder:

```bash
git clone <libcoopa repo> libcoopa
git clone <sfxcoopa repo> sfxcoopa
```

### 2. Dependencies

Everything else is vendored or comes through libcoopa:

| Dependency | Source | Used for |
|---|---|---|
| libcoopa (`coopa::lib`) | sibling folder `../libcoopa` | `Signal`, `Logger`, `YAMLMap`, `Transform`, file paths |
| glm | through libcoopa | vectors and matrices for 3D audio |
| miniaudio | vendored in `includes/miniaudio/` | sound card output and WAV/MP3 decoding |

miniaudio's implementation is compiled once, in `src/sfx_impl.cpp`. `coopa::sfx` passes
`MA_NO_*` defines that turn off its engine, node graph, resource manager, generation and FLAC
parts.

### 3. Build

You need CMake 3.20 or newer and a C++20 compiler:

```bash
cmake -B build && cmake --build build -j
```

To use the library from another CMake project, add it and link the `coopa::sfx` target:

```cmake
add_subdirectory(path/to/sfxcoopa ${CMAKE_BINARY_DIR}/sfxcoopa-build)
target_link_libraries(my_game PRIVATE coopa::sfx)
```

Do not define `MINIAUDIO_IMPLEMENTATION` yourself. `coopa::sfx` already links the one
implementation and passes the matching `MA_NO_*` defines to your code.

### 4. Play a sound

`AudioEngine` is the one object you create. `AudioDevice` connects it to the sound card.

```cpp
#include <sfxcoopa/core/engine.h>
#include <sfxcoopa/core/device.h>

using namespace coopa::sfx;

core::AudioEngine engine(data::AudioFormat{48000, 2});

core::MixerBus& music = engine.create_bus("Music", engine.master());
core::MixerBus& sfx = engine.create_bus("SFX", engine.master());
music.set_volume_db(-6.0f);

// A clip must be mono to play in 3D. Save that in its sidecar once.
data::ClipImportSettings whoosh;
whoosh.force_mono = true;
whoosh.default_bus = "SFX";
whoosh.save("sounds/whoosh.wav");  // writes sounds/whoosh.wav.import

engine.play("sounds/theme.wav", {.bus_name = "Music"});

spatial::Listener listener;  // at the origin, facing -Z, +Y up
engine.set_listener(listener);
core::VoiceHandle h = engine.play_3d("sounds/whoosh.wav", glm::vec3(3.0f, 0.0f, -2.0f));

engine.on_voice_finished.connect([](core::VoiceHandle) { /* ... */ });

// The device calls this from its audio thread.
core::AudioDevice device(core::DeviceConfig::from_env(),
                         [&](float* out, uint32_t frames) { engine.render_offline(out, frames); });

// Once per game frame, on the main thread:
engine.set_voice_position(h, glm::vec3(2.0f, 0.0f, -3.0f));
engine.update(dt);
```

To render without a sound card, skip the device and call `engine.render_offline(buffer,
frames)` yourself (up to 4096 frames per call by default). The output is the same as the
device path.

### 5. Run the demo

`sfxcoopa_playback` plays `ex/SEFE_Flashback.wav` on a Music bus and orbits
`ex/SEFE_Whoosh03.wav` around the listener. Use headphones to hear the binaural effect.

```bash
MAX_SECONDS=10 ./build/sfxcoopa_playback       # play out loud for 10 seconds
BOUNCE=orbit.wav ./build/sfxcoopa_playback      # render to output/orbit.wav, no device
SFX_DEVICE=null ./build/sfxcoopa_playback       # silent null backend
```

## Testing

```bash
ctest --test-dir build                 # every suite, in parallel (label: unit)
./build/sfxcoopa --list                # or run the test executable directly
./build/sfxcoopa --suite voice         # one suite
```

The tests live in `tests/`, one suite per system in `tests/<suite>_test.cpp` (`db`, `ma_check`,
`ring_buffer`, `dsp_filters`, `resampler`, `spatial`, `import_settings`, `decoder`, `audio_clip`,
`mixer_bus`, `voice`, `mixer`, `engine`, `audio_source`, `audio_listener`, `device`), written
against libcoopa's `coopa/testing/test.h` framework. Each suite is its own ctest entry
(`sfxcoopa_<suite>`). Shared fixtures are in `tests/support/`. They open no real audio device:
audio goes through `render_offline()`, and the device test uses miniaudio's null backend. Clips
are decoded from per-test copies of the `ex/` fixtures in a temp directory, so a `.import`
sidecar next to a committed fixture cannot change a result and no test writes into the repo.
Set `SFX_DEVICE=null` to make any program built on `DeviceConfig::from_env()` silent.

## Tools

`sfxcoopa_bake` generates a small library of synthesized UI and game sound effects (clicks,
toggles, sweeps, whooshes, hits) as WAV files plus a `sounds.yaml` manifest:

```bash
./build/sfxcoopa_bake path/to/assets/sounds
```

Every preset is a few numbers in `tools/bake_ui_sfx.cpp`, so the output can be regenerated.

## Project layout

```
sfxcoopa/
├── core/         AudioEngine, PlayParams, AudioDevice, DeviceConfig
├── mixer/        Mixer, MixerBus, Voice, VoicePool, VoiceHandle, commands and events
├── components/   AudioSource, AudioListener, SfxResources (scene integration)
├── spatial/      Listener, AttenuationCurve, SpatialParams, BinauralPanner
├── data/         AudioFormat, Decoder, AudioClip, ClipImportSettings, smpl chunk reader
├── dsp/          IEffect, Biquad, OnePoleLowpass, DelayLine, Resampler
├── collections/  lock-free SPSC RingBuffer
└── util/         dB conversion, miniaudio error checks
src/              sfx_impl.cpp, the single miniaudio implementation unit
includes/         vendored miniaudio
tools/            bake_ui_sfx.cpp (sfxcoopa_bake)
ex/               sample WAV and MP3 clips used by the tests and demo
plans/            design notes and roadmap
sfx_yaml.h        (in sfxcoopa/) SceneLoader registration for the scene components
tests/            headless test suites (<suite>_test.cpp) and shared fixtures (support/)
demos/            demo_playback.cpp (sfxcoopa_playback), the audible and bounce demo
```

All code lives in `namespace coopa::sfx`. Headers are included by path, for example
`<sfxcoopa/core/engine.h>`. There is no umbrella header.

## Platform notes

- **Backends.** miniaudio picks the platform backend (CoreAudio on macOS, ALSA or PulseAudio on
  Linux, WASAPI on Windows). The library is developed on Linux, and the test suite also passes
  on macOS.
- **Threads.** Voice control goes through a lock-free command queue, so `play()`, `stop()` and
  friends are safe while a device is running. Bus volume, mute and solo are plain fields, not
  atomics. Call `update()` and every engine method from one main thread.
- **Mono for 3D.** `play_3d()` throws on a stereo clip. Set `force_mono` in its import settings.
- **Per-call versus per-clip.** `PlayParams` sets gain, pitch, priority and bus for each call.
  The sidecar's `volume`, `pitch` and `priority` fields are saved and loaded, but `play()` does
  not apply them. `AudioSource` does apply `volume` and `pitch`. `PlayParams::loop` and
  `PlayParams::spatial` optionally override the clip's loop flag and spatial defaults.
- **Standalone targets.** The test, demo and bake executables are only built when sfxcoopa is
  the top-level CMake project. A parent project that adds it with `add_subdirectory()` gets only
  `coopa::sfx`.

## Roadmap

These are designed in [`plans/`](plans/) but not built yet:

- Streaming playback from disk. `LoadType::Streaming` exists as a setting, but every clip is
  currently decoded fully into memory.
- Ready-made effects: filters, delay, reverb and a limiter for the `IEffect` chain.
- An HRTF panner option.

## Documentation

- Every header has `@file`, `@brief` and per-member doc comments that explain the threading
  and ramping rules.
- `.docs/` is the generated HTML API reference, built from those comments with
  `coopadocs build`.
- [`plans/2026-08-17_sfxcoopa-audio-engine.md`](plans/2026-08-17_sfxcoopa-audio-engine.md)
  holds the design and the reasons behind it.
