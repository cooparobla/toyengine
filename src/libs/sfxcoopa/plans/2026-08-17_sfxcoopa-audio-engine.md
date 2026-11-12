# sfxcoopa — Header-Only Game Audio Engine

## Context

`/home/coopa/git/sfxcoopa` currently holds `.git/`, this `plans/` doc, and the two committed `ex/` wav fixtures — everything else below is still to be built. It needs to become the audio sibling of `libcoopa` / `gfxcoopa` / `uicoopa`: a header-only C++20 library that plays wav/mp3, mixes many concurrent tracks at independent levels, and spatializes sources binaurally for a game engine — usable from `blendy` alongside the existing stack.

**2026-08-29 revalidation.** The design below was re-checked against the actual sibling sources, the vendored miniaudio header, and the house CMake/test conventions as they exist today (not as of this doc's original writing). Several facts changed; corrections are called out inline as **`[REVALIDATED]`** blocks rather than silently rewritten, so the reasoning that led to the original design stays visible.

The three sibling repos share one rigid house style, verified by reading them:

| Convention | Value |
|---|---|
| Layout | `<repo>/<repo>/` holds all headers; one dir per module |
| Headers | 100% `.h`, all `inline`, **no umbrella header**, no `.cpp` |
| Guards | `#ifndef SFXCOOPA_<DIR>_<FILE>_H` — never `#pragma once` |
| Namespace | nested blocks `namespace coopa { namespace sfx { namespace core {`, never `a::b::c` |
| Naming | `PascalCase` types · `snake_case` methods · public members bare · private members and private methods trailing `_` · `enum class` PascalCase · `inline constexpr k_snake_case` |
| Getters | bare noun (`position()`), setters `set_x()` |
| Errors | `throw std::runtime_error("[sfxcoopa] ...")` — no `Result<T>` exists in the stack |
| Logging | `coopa::debug::Logger("Audio")` |
| Docs | `@file`/`@brief` block per file, `@class`+`@code` per type, trailing `/**< ... */` per member |
| Third party | vendored + committed under `<repo>/includes/<lib>/`, implementation compiled once |
| CMake | no `install()`, no `find_package` support — see **`[REVALIDATED]`** in Phase 0 for how the implementation TU and consumability now work |
| Tests | hand-rolled `test.cpp` at repo root with `RUN_TEST`/`ASSERT_*` macros, registered via `enable_testing()`/`add_test` |

**`[REVALIDATED]`** The "no `add_library`, consumers just `include_directories()`" row was true of uicoopa/caml (pure headers, nothing to compile) but is no longer how a repo with a vendored *implementation* works. gfxcoopa — written after uicoopa — ships an `INTERFACE` target aliased `coopa::gfx` plus a `STATIC` target (`gfxcoopa_impl`) compiling its vendored implementation blocks in exactly one TU, and libcoopa mirrors this with `coopa::lib`. Since miniaudio needs a linked implementation too, sfxcoopa must follow the gfxcoopa shape, not the older uicoopa one — see Phase 0.

**Dependency direction:** sfxcoopa depends on **libcoopa only** (glm, `event::Signal`, `scene::Component`, `debug::Logger`, `collections::YAMLMap`, `job::Thread`) plus vendored miniaudio. It does **not** depend on gfxcoopa or uicoopa — integration with those happens app-side, which stays trivial because sources are `coopa::scene::Component`s emitting `coopa::event::Signal`s. Glue headers were explicitly deferred.

### Settled design decisions

- **miniaudio for device + decode only.** Vendor `includes/miniaudio/miniaudio.h` (single file, public domain, ALSA/Pulse/WASAPI/CoreAudio, built-in wav/mp3/flac). sfxcoopa uses `ma_device` (raw PCM callback) and `ma_decoder` (bytes → PCM) and nothing else. `ma_engine` / `ma_sound` / `ma_spatializer` are **not** used — the mixer, voice pool, resampler and all 3D math are ours, in glm.
- **Analytic binaural panning:** Woodworth ITD via fractional delay + per-ear head-shadow lowpass + ILD, plus distance curves, cones and doppler. No HRIR dataset, no FFT, no binary assets. An `IPanner` seam is left so an HRTF panner can be dropped in later.
- **In scope:** streaming long files, per-bus DSP effect chain, YAML scene components + `.import` sidecar settings.

### Example assets — `ex/` (present in the repo)

Two files are committed and are the suite's real fixtures. Their actual headers, parsed chunk by chunk:

| File | Format | Rate | Ch | Frames | Duration | Bytes |
|---|---|---|---|---|---|---|
| `ex/SEFE_Whoosh03.wav` | **IEEE float32** (`fmt` tag 3) | 44 100 | 2 | 31 903 | 0.723 424 s | 255 448 |
| `ex/SEFE_Flashback.wav` | **IEEE float32** (`fmt` tag 3) | 44 100 | 2 | 156 468 | 3.548 027 s | 1 251 968 |

Four properties of these files change the plan, and none of them were assumptions I'd have made:

1. **They are 32-bit float, not 16-bit PCM.** Python's `wave` module refuses them outright (`unknown format: 3`); miniaudio handles them natively. The upside is large: since the engine's internal format is also f32, a load at matching rate is a **pure memcpy with no quantization**, so the decode test can assert **bit-exact** equality against the raw `data` chunk rather than a tolerance. That is a far stronger test than `ASSERT_NEAR`.
2. **They are 44.1 kHz while the engine defaults to 48 kHz.** Every load therefore exercises the resampler — good coverage by default, but it means the "bit-exact" assertion above only holds when the test engine is explicitly constructed at 44 100. The suite does both: one engine at 44 100 for exactness, one at 48 000 for the resample path (expected `31903 × 48000/44100 ≈ 34724` frames, ±1).
3. **They are stereo, and a stereo source cannot be spatialized.** ITD/ILD are defined for a point source; feeding two channels into a binaural panner is meaningless. This forces a rule the plan previously left implicit — see *Mono requirement* in Phase 5.
4. **Both carry `smpl`, `inst` and `acid` chunks, and lead with a `JUNK` chunk.** The `smpl` chunks are 36 bytes = the header with **zero** loop points, so no embedded loops here — but the format supports them, and `smpl` is where a sound designer's loop markers live. Phase 2 reads them as loop defaults. The leading `JUNK` chunk (a padding block Pro Tools and Reaper emit) is the classic breaker of naive parsers that assume `fmt ` sits at offset 12; miniaudio walks chunks properly, but any hand-written probe in the suite must too.

**Still missing: an mp3 fixture.** The mp3 decode path has no test asset. Add one short mp3 to `ex/` — converting `SEFE_Whoosh03.wav` is the obvious choice. Note that its test cannot assert exact frame counts: mp3 is lossy and encoders add ~1 152 frames of decoder delay plus end padding, so assert duration within one MPEG granule and correlate the decoded waveform against the wav rather than comparing samples.

**`[REVALIDATED]`** No `ffmpeg`/`lame`/`sox` CLI is installed on this box, but `gst-launch-1.0` with the `lamemp3enc` GStreamer plugin is, and is sufficient:
```bash
gst-launch-1.0 filesrc location=ex/SEFE_Whoosh03.wav ! wavparse ! audioconvert \
  ! lamemp3enc ! filesink location=ex/SEFE_Whoosh03.mp3
```
If `wavparse` balks at the float32 `fmt` tag, fall back to feeding the raw `data` chunk through `rawaudioparse` (`format=pcm`, `pcm-format=f32le`, `sample-rate=44100`, `num-channels=2`) instead of `wavparse`. Generate this in Phase 0 (U0 in the build sequence below), not alongside the decoder work in Phase 2 — it depends on an external tool and its own iteration shouldn't block decoder tests.

---

## Architecture

```
                    main thread                        audio callback thread
  AudioEngine::update(dt) ──┐                     ┌── AudioDevice (ma_device)
    push commands ──────────┼─► CommandRing ─────►│      Mixer::render(out, frames)
    drain events  ◄─────────┼─── EventRing ◄──────┤        Bus graph (post-order)
    emit Signal<>           │                     │          Voice × N
                            │                     │            Resampler → Panner → Bus
  decode thread             │                     └──────────────────────────────────
    AudioStream::pump() ────┴─► PCM RingBuffer ────────────────► (read-only on audio thread)
```

**Audio-thread rules — non-negotiable, stated in every relevant `@file` block:** no locks, no allocation, no exceptions, no logging, no `std::function` construction. This rules out `coopa::job::ParallelQueue`/`ParallelVector` (mutex-based) and `coopa::event::Signal` (not thread-safe, documented as such) *on the audio thread* — hence our own SPSC ring both directions, with `Signal::emit()` happening only on the main thread inside `update(dt)`.

**Offline render path is a first-class feature, not a test hack:** `Mixer::render(float* out, uint32_t frames)` is pure and hardware-free, so the entire library is testable in CI with no sound card. `AudioDevice` is a thin shell that calls it.

### File tree to create

```
sfxcoopa/
├── CMakeLists.txt · README.md · .coopadocs · .gitignore
├── test.cpp                          headless suite (no device opened)
├── test_playback.cpp                 audible demo, env-gated
├── configuration/root_directory.h.in
├── includes/miniaudio/miniaudio.h    vendored, committed
├── ex/                               test fixtures: 2 float32 wavs (present) + 1 mp3 (to add)
│                                     + .import sidecars written by the suite
├── plans/2026-08-17_sfxcoopa-audio-engine.md
└── sfxcoopa/
    ├── util/          error.h · db.h                          coopa::sfx::util
    ├── collections/   ring_buffer.h                           coopa::sfx::collections
    ├── data/          audio_format.h · import_settings.h · decoder.h
    │                  wav_chunks.h · audio_clip.h · audio_stream.h  coopa::sfx::data
    ├── dsp/           one_pole.h · biquad.h · delay_line.h · resampler.h · effect.h
    │   └── effects/   lowpass.h · highpass.h · delay.h · reverb.h · limiter.h
    │                                                          coopa::sfx::dsp[::effects]
    ├── spatial/       attenuation.h · listener.h · panner.h   coopa::sfx::spatial
    ├── mixer/         voice.h · bus.h · mixer.h               coopa::sfx::mixer
    ├── core/          device.h · engine.h                     coopa::sfx::core
    ├── components/    audio_source.h · audio_listener.h       coopa::sfx::components
    └── sfx_yaml.h     register_sfx_components() + SfxResources  coopa::sfx
```

---

## Phase 0 — Repo scaffolding

1. Save this plan to `/home/coopa/git/sfxcoopa/plans/2026-08-17_sfxcoopa-audio-engine.md`. *(done)*
2. `.coopadocs` → `include:\n  - sfxcoopa`. `.gitignore` → `build/`, `output/`, `.docs/` — **must not ignore `ex/`**, which is committed fixture data, not build output.
3. `configuration/root_directory.h.in` — copy verbatim from `/home/coopa/git/libcoopa/configuration/root_directory.h.in`. Note it resolves `ROOT_DIR` from `@CMAKE_SOURCE_DIR@` (the top-level project, not `@CMAKE_CURRENT_SOURCE_DIR@`) — a latent quirk shared by every sibling that only matters if something else ever `add_subdirectory()`s sfxcoopa itself; harmless here since blendy consumes it via its own `configuration/`.
4. Vendor `miniaudio.h` (single file, choice of public-domain or MIT-0) into `includes/miniaudio/`. Include it as `<miniaudio/miniaudio.h>`.

   **`[REVALIDATED]`** Confirmed by downloading and inspecting the header directly: current release is **v0.11.25** (2026-03-04), 95 864 lines. Single-header `MINIAUDIO_IMPLEMENTATION` mode (guarded at line ~4030/11542/95813) is still fully supported. Every API this design needs is present and matches the signatures used below: `ma_decoder_config_init(format, channels, sampleRate)`, `ma_decoder_init_file` / `_init_memory` / `_read_pcm_frames` / `_seek_to_pcm_frame` / `_get_data_format` / `_get_length_in_pcm_frames` / `_get_cursor_in_pcm_frames` / `_uninit`, `ma_device_config_init`, `ma_device_init` / `_start` / `_uninit`, `ma_context_init`, `ma_backend_null`, and `ma_result_description(ma_result)` returning `const char*` (so `util/error.h` should call this directly rather than hand-rolling a string table). Also present: `ma_encoder_*` for `BOUNCE=out.wav` in Phase 6's demo. Third-party version table for this doc: **miniaudio 0.11.25**, and (via `coopa::lib`) **glm 0.9.9.3**, **fkYAML 0.4.2**, **parallel_hashmap 2.0.0**.

5. `CMakeLists.txt`.

   **`[REVALIDATED]`** The block originally sketched here (modelled on uicoopa, `add_definitions(-DMINIAUDIO_IMPLEMENTATION)`, plain `include_directories()`, no library target) has two problems once checked against the current sibling shape: (a) it compiles miniaudio's ~96k-line implementation into *both* `test.cpp` and `test_playback.cpp`, doubling build time and risking duplicate-symbol issues if a TU ever pulls in both; (b) with no `add_library`/alias, **blendy cannot link sfxcoopa at all** — miniaudio needs a compiled implementation, unlike uicoopa/caml's pure-header vendored libs, so bare `include_directories()` leaves every `ma_*` symbol undefined at blendy's link step. gfxcoopa already solved exactly this (volk/VMA/stb) with a single `STATIC` impl target plus an `INTERFACE` consumable target; sfxcoopa follows that shape:

   ```cmake
   cmake_minimum_required(VERSION 3.20)
   cmake_policy(VERSION 3.20)
   project(sfxcoopa)
   set(CMAKE_CXX_STANDARD 20)
   set(CMAKE_CXX_STANDARD_REQUIRED ON)
   set(CMAKE_CXX_EXTENSIONS OFF)

   get_filename_component(ROOT_DIR_PARENT "${CMAKE_SOURCE_DIR}" DIRECTORY)
   set(LIBCOOPA_DIR "${ROOT_DIR_PARENT}/libcoopa")
   if(NOT TARGET coopa::lib)
       add_subdirectory(${LIBCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/libcoopa-build)
   endif()

   configure_file(configuration/root_directory.h.in configuration/root_directory.h)

   # The ONE translation unit that compiles miniaudio's implementation.
   add_library(sfxcoopa_impl STATIC src/sfx_impl.cpp)
   target_compile_features(sfxcoopa_impl PRIVATE cxx_std_20)
   target_include_directories(sfxcoopa_impl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/includes)
   target_compile_definitions(sfxcoopa_impl PRIVATE MINIAUDIO_IMPLEMENTATION)
   target_link_libraries(sfxcoopa_impl PUBLIC ${CMAKE_DL_LIBS} pthread m)

   # coopa::sfx — the public, header-only consumable. Named sfxcoopa_lib (not
   # sfxcoopa) because cplay resolves the run binary as ./build/<project name>.
   add_library(sfxcoopa_lib INTERFACE)
   add_library(coopa::sfx ALIAS sfxcoopa_lib)
   target_compile_features(sfxcoopa_lib INTERFACE cxx_std_20)
   target_include_directories(sfxcoopa_lib INTERFACE
       ${CMAKE_CURRENT_SOURCE_DIR}
       ${CMAKE_CURRENT_SOURCE_DIR}/includes
       ${CMAKE_CURRENT_BINARY_DIR}/configuration)
   # MA_NO_* must be IDENTICAL in sfxcoopa_impl and every consumer TU, or
   # miniaudio's structs change shape across TUs (ODR violation). Declaring
   # them INTERFACE here, inherited by every consumer, guarantees that.
   target_compile_definitions(sfxcoopa_lib INTERFACE
       MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION MA_NO_FLAC)
   target_link_libraries(sfxcoopa_lib INTERFACE sfxcoopa_impl coopa::lib)

   add_executable(sfxcoopa test.cpp)
   target_link_libraries(sfxcoopa PRIVATE coopa::sfx)
   add_executable(sfxcoopa_playback test_playback.cpp)
   target_link_libraries(sfxcoopa_playback PRIVATE coopa::sfx)

   enable_testing()
   add_test(NAME sfxcoopa_tests COMMAND sfxcoopa)
   ```

   `MA_NO_ENCODING` is deliberately **not** in the trim list — the playback demo's `BOUNCE=out.wav` needs `ma_encoder`. `sfxcoopa_impl` links `pthread`/`m`/`${CMAKE_DL_LIBS}` **PUBLIC** because miniaudio's inline helpers compile into consumer TUs too, mirroring gfxcoopa's rationale for linking `glfw` PUBLIC on `gfxcoopa_impl`. `src/sfx_impl.cpp` is a pure include list (`#include <miniaudio/miniaudio.h>`), modelled verbatim on `/home/coopa/git/gfxcoopa/src/gfx_impl.cpp` — the macros live in CMake, not the source file.

   **Also linking against `coopa::lib` (not raw `include_directories`) matters beyond consumability**: `libcoopa_lib` sets `GLM_FORCE_DEPTH_ZERO_TO_ONE` and `GLM_FORCE_RADIANS` as `INTERFACE` compile definitions. The original plan's `include_directories(${LIBCOOPA_DIR}/includes/ ${LIBCOOPA_DIR})` would silently drop both, giving sfxcoopa different `glm::` semantics from every other repo in the workspace. Separately: `GLM_ENABLE_EXPERIMENTAL` currently leaks into most TUs via `coopa/util/transform.h` (it `#define`s it and never `#undef`s); don't rely on that leak — if sfxcoopa includes any `glm/gtx/*` header, `#define GLM_ENABLE_EXPERIMENTAL` itself first.

   **blendy consumption (documented for the record; blendy is not modified by this plan):**
   ```cmake
   set(SFXCOOPA_DIR "${ROOT_DIR_PARENT}/sfxcoopa")
   if(NOT TARGET coopa::sfx)
       add_subdirectory(${SFXCOOPA_DIR} ${CMAKE_CURRENT_BINARY_DIR}/sfxcoopa-build)
   endif()
   # ...
   target_link_libraries(blendy PRIVATE coopa::gfx coopa::sfx ...)
   ```

## Phase 1 — Foundations

- **`util/error.h`** — mirrors `gfxcoopa/util/error.h`'s shape: `inline void ma_check(ma_result, const std::string& context)` throwing `std::runtime_error("[sfxcoopa] miniaudio error in " + context + ": " + ma_result_description(result))`, and `#define SFX_MA_CHECK(expr) ::coopa::sfx::util::ma_check((expr), #expr)`. **`[REVALIDATED]`**: miniaudio itself ships `ma_result_description(ma_result) -> const char*` with every result code covered, so there is no need for sfxcoopa to hand-roll the switch statement gfxcoopa's `vk_result_string` does for Vulkan (which has no such built-in) — just call it.
- **`util/db.h`** — `inline float db_to_linear(float)`, `linear_to_db(float)`, `inline constexpr float k_silence_db = -80.0f`.
- **`collections/ring_buffer.h`** — `template<typename T> class RingBuffer`: lock-free SPSC, power-of-two capacity, cache-line-padded head/tail atomics reusing `coopa::job::k_cache_line_size` from `<coopa/job/platform.h>` (confirmed present, `alignas(k_cache_line_size)` is the idiom — `work_stealing_deque.h`'s own `Slot` struct claims padding in its doc comment but has none; don't copy that bug, pad for real). API: `bool push(const T&)`, `bool pop(T&)`, `uint32_t write(const T* src, uint32_t n)`, `uint32_t read(T* dst, uint32_t n)`, `available_read/write()`, `capacity()`, `clear()`. Two instantiations carry the whole thread model: `RingBuffer<Command>` and `RingBuffer<float>`.
- **`data/audio_format.h`** — `enum class SampleFormat { U8, S16, S24, S32, F32 }`, `struct AudioFormat { uint32_t sample_rate = 48000; uint32_t channels = 2; }`, `inline constexpr uint32_t k_default_sample_rate = 48000`, `k_control_block_frames = 64` (parameter-ramp granularity), `k_max_voices = 128`.

## Phase 2 — Decoding, clips, import settings

- **`data/decoder.h`** — `class Decoder`: RAII over `ma_decoder`, ctor from filesystem path (resolved via **`FileUtil::get_root_path`**, global scope, from `<coopa/util/file.h>`) or from a memory span. **`[REVALIDATED]`**: the original plan named `FileUtil::get_asset_path`, but that function only searches under an `assets/` prefix (it strips a leading `assets/` from the input and looks in `ROOT_DIR/assets/…` then `PROJ_DIR/assets/…`) — it would never find anything under `ex/`. `get_root_path(path)` is the plain `ROOT_DIR + "/" + path` resolver (honouring the `LOGL_ROOT_PATH` env override) and is what actually locates `ex/SEFE_Whoosh03.wav`. Also note `FileUtil` is **not** in `coopa::util` — it's a global-scope class, unlike `Transform`/`Time` which are. `read_frames(float* out, uint64_t n)`, `seek_to_frame(uint64_t)`, `total_frames()`, `format()`. Configured to always output f32 at the engine rate so the format zoo stops here. Also exposes `source_format()` (the file's own rate/channels/sample format, pre-conversion) — the tests need it to distinguish "decoded correctly" from "resampled correctly", and it's what tells `AudioClip` whether the f32-at-matching-rate memcpy fast path applies.
- **`data/wav_chunks.h`** — a ~60-line standalone RIFF chunk walker for the metadata miniaudio's decoder doesn't surface: `struct SampleLoop { uint32_t start, end; }` and `inline std::vector<SampleLoop> read_smpl_loops(const std::string& path)`. It must **walk the chunk list** rather than assuming `fmt ` at offset 12 — both `ex/` files lead with a `JUNK` chunk — and honour the odd-size padding byte between chunks. Used by `ClipImportSettings::load` to seed `loop_start_frame`/`loop_end_frame` from the sound designer's markers.
- **`data/import_settings.h`** — the import architecture, matching the stack's "defaults struct as a trailing defaulted parameter" idiom (cf. `gfxcoopa`'s `PipelineConfig`):
  ```cpp
  enum class LoadType { DecompressOnLoad, Streaming };
  struct ClipImportSettings {
      LoadType load_type = LoadType::DecompressOnLoad;
      bool force_mono = false;                /**< Downmix to 1 channel at import. */
      uint32_t sample_rate_override = 0;      /**< 0 = keep source rate. */
      float volume = 1.0f;                    /**< Baked-in gain applied on top of voice gain. */
      float pitch = 1.0f;
      bool loop = false;
      uint64_t loop_start_frame = 0;
      uint64_t loop_end_frame = 0;            /**< 0 = end of clip. */
      std::string default_bus = "Master";
      int priority = 0;                       /**< Higher steals over lower when the voice pool is exhausted. */
      bool spatialize = false;
      spatial::SpatialParams spatial{};       /**< Defaults used when spatialize is true. */
      static ClipImportSettings load(const std::string& clip_path);  // reads "<clip_path>.import"
      void save(const std::string& clip_path) const;
  };
  ```
  `priority` added over the original sketch — Phase 4's voice-stealing rule ("lowest priority, then oldest") needs a field to read; `PlayParams` (Phase 6) gets the same field so a call site can override the import default per-play.

  Sidecar parse/write via `coopa::collections::YAMLMap` (`<coopa/collections/yaml_map.h>`). **Resolution order: explicit argument > `.import` sidecar > embedded `smpl` loop points > struct defaults** — a missing sidecar is not an error, and neither is a missing `smpl` chunk.

  **`[REVALIDATED]`** `YAMLMap` has sharper edges than "just serialize the struct":
  - `YAMLMap::load()` on a missing file logs and returns an **empty mapping rather than throwing** — confirms the "missing sidecar is not an error" requirement needs no special-casing. A genuine *parse* error on a malformed file **does** rethrow; wrap that call.
  - `get<T>(key, default_value)` has no single-argument overload — a default is mandatory, which is convenient here since every field already has one.
  - `get_node()` returns the nested value **by value** (a detached copy); writes into it never propagate back to the parent. This makes a nested `spatial:` block awkward to round-trip correctly with `save()`. **Use flat, prefixed keys instead** — `spatial_position_x/y/z`, `spatial_min_distance`, `spatial_max_distance`, `spatial_rolloff`, `spatial_doppler_factor`, `spatial_inner_cone_angle`, etc. — so both `load()` and `save()` are simple flat `get`/`set` calls with no nested-node lifetime trap.
  - `save()` throws if the file can't be opened and does **not** create parent directories — the caller (`ClipImportSettings::save`) is responsible for the sidecar's directory already existing, which it does since it sits next to the source clip.
- **`data/audio_clip.h`** — `class AudioClip`: fully decoded interleaved f32 PCM in a `std::vector<float>`, plus `frame_count()`, `format()`, `settings()`, `data()`. `static AudioClip load(const std::string& path, const AudioFormat& engine_format, const ClipImportSettings& settings = {})` applies force-mono / rate conversion / channel conversion at load. **`[REVALIDATED]`**: the terse original sketch omitted `engine_format` — but the test matrix explicitly requires loading the *same* clip at two different engine rates (44100 for the bit-exact fast path, 48000 for the resample path), and `AudioClip::load` is a free-standing static factory with no implicit "current engine" to read a rate from. `AudioEngine::load_clip` (Phase 6) passes its own `DeviceConfig`-derived format down. Non-copyable, movable. Shared via `std::shared_ptr<AudioClip>`; cache keyed by logical path in the engine, exactly as `SceneLoader`'s `mesh_cache` does.
  **Fast path:** when the source is already f32 at the engine rate with the requested channel count — true for both `ex/` files on a 44 100 Hz engine — the load is a straight copy with no conversion stage. Worth an explicit branch: it is the common case for a game that ships assets at the engine's rate, and it's what makes the bit-exact decode test meaningful.

## Phase 3 — DSP primitives

- **`dsp/one_pole.h`** — `class OnePoleLowpass { void set_cutoff(float hz, uint32_t rate); float process(float); void reset(); }`. Used for head shadow and parameter smoothing.
- **`dsp/biquad.h`** — RBJ cookbook coefficients: `static Biquad lowpass/highpass/peaking/lowshelf/highshelf(...)`, `float process(float)`, `void reset()`.
- **`dsp/delay_line.h`** — `class DelayLine`: circular f32 buffer with **fractional** read (linear interpolation) for ITD, `set_delay_samples(float)`, `push(float)`, `read()`. Interpolation is what makes sub-sample ITD — and therefore smooth head rotation — work at all.
- **`dsp/resampler.h`** — `class Resampler`: cubic-Hermite fractional-position reader driving both sample-rate conversion and pitch, `set_ratio(double)`, `process(const float* in, uint32_t in_frames, uint32_t& in_consumed, float* out, uint32_t out_frames, uint32_t& out_written)`.

  **`[REVALIDATED]`** the out-param counts (over the original plain `process(in, in_frames, out, out_frames)`) are not cosmetic: cubic Hermite needs taps at input positions `-1..+2` relative to its fractional read cursor, so the class must own persistent state — a small history buffer plus a `double` fractional phase — that survives across calls, and callers on both ends (`Voice::render_add`, pulling exactly `out_frames`; `AudioClip::load`, pushing a whole file through in one or more chunks) need to know how much of each buffer was actually touched. The de-risking test for this primitive: feed one sine through in 1-frame, 7-frame, and 4096-frame chunks and assert the three outputs are bit-identical — chunk-boundary bugs are the single likeliest defect in this file.
- **`dsp/effect.h`** — `class IEffect { virtual void prepare(const AudioFormat&, uint32_t max_frames) = 0; virtual void process(float* buffer, uint32_t frames, uint32_t channels) = 0; virtual void reset() {} virtual ~IEffect() = default; }`. In-place, pre-allocated in `prepare()`, allocation-free in `process()`.

## Phase 4 — Voices, buses, mixer

This is the load-bearing phase. **`[REVALIDATED]`**: libcoopa has no ready-made generational handle or free-list pool to copy — `coopa::job::JobHandle` is `(CounterPool*, uint32_t index)` with **no generation field at all** (a stale handle silently aliases a recycled slot), and `coopa::job::CounterPool` is a bump allocator with **no `free()`/`release()`** (only a bulk `reset()`, documented as safe "only when all previously-issued handles are known to be complete"). So `VoiceHandle`/`VoicePool` below are new code; only method-naming conventions are borrowed from those two, not their mechanics. This also means the cross-thread allocation problem — who assigns a handle, and when is it safe to recycle a slot — has no existing answer to fall back on, which is why it's settled explicitly:

- **Voice allocation is main-thread-only.** `AudioEngine::play()` pops an index from a main-thread-owned free list and bumps that slot's generation counter itself, then sends `Play{index, generation, ...}` over the command ring. **The audio thread never allocates a slot and never bumps a generation** — it only checks that a command's generation matches the voice's current one, dropping it silently otherwise (handles a `stop()` racing a `play()` reusing the same slot). When a voice reaches `Free` inside `render()`, the audio thread pushes `Freed{index}` on the event ring; `update(dt)` is what returns the index to the main-thread free list. Voice stealing, with the free list empty, is likewise decided main-side: `play()` picks a victim from the engine's own shadow copy of voice state (lowest `priority`, then oldest `started_at`), bumps that slot's generation, and issues `Play` for it — the audio thread, seeing a `Play` land on an occupied slot, runs the normal ~5 ms fade-out before restarting rather than cutting. This keeps every allocation/generation mutation on one thread and removes the class of races a lock-free scheme would otherwise need CAS loops for.
- **`mixer/voice.h`**
  - `struct VoiceHandle { uint32_t index = k_invalid_voice; uint32_t generation = 0; bool is_valid() const; }` — generational, so a stale handle can never stop a recycled voice.
  - `enum class VoiceState { Free, Playing, Paused, Stopping };`
  - `class Voice` — holds source (`AudioClip*` or `AudioStream*`), playback cursor, `Resampler`, per-voice gain/pitch, loop points, target bus index, optional `BinauralPanner`, and **smoothed** parameter targets. `render_add(float* out, uint32_t frames)` mixes additively. Even though streaming (`AudioStream`) is Phase 7, the source member should be a small tagged `SourceRef` from the start — retrofitting a second source kind into a finished `render_add` later is a bigger rewrite than building the seam now costs.
  - `class VoicePool` — fixed `k_max_voices` array, main-thread free list (see above), `allocate()`/`release()`. Zero allocation after construction.
  - **Click-free ramping:** every gain/delay/pitch change is a *target*; the voice ramps toward it once per `k_control_block_frames` sub-block over ~10 ms. Stops fade out over ~5 ms rather than cutting. Without this, every parameter change pops. Write this sub-block chunking loop before any per-sample DSP is added to `render_add` — adding it after the fact means touching every line of that function.
- **`mixer/bus.h`** — `class MixerBus` = the "track" concept: `name()`, `index()` (stable `uint32_t` assigned at `create_bus()`; buses are never destroyed for the engine's lifetime, which is what lets commands address a bus by index instead of by string), `volume()`/`set_volume(float)` (linear, with `set_volume_db()`), `mute`/`solo`, `add_effect(std::unique_ptr<IEffect>)`, `parent()`, `children()`, and its own scratch buffer sized `max_frames * channels` in `prepare()`, unconditionally zeroed at the top of every render (not conditionally — a stale-scratch bug is easy to introduce and easy to miss). Render is **depth-first post-order**: children render into their scratch, effects run in chain order, result is accumulated into the parent scaled by bus gain. Solo is resolved control-side into a per-bus `effective_mute_` flag so the audio thread never scans the graph.
- **`mixer/mixer.h`** — `class Mixer`: owns the `VoicePool` and the bus graph rooted at `"Master"`, plus the two rings. `render(float* out, uint32_t frames)` is the single audio-thread entry point: drain `CommandRing` → render voices into buses → render bus graph post-order → write master to `out` → push completion/loop events to `EventRing`. `create_bus(name, parent)`, `find_bus(name)`.
- **`Command`** — POD tagged union in `mixer.h` (`Play`, `Stop`, `Pause`, `SetVoiceGain`, `SetVoicePitch`, `SetVoiceSpatial`, `SetBusVolume{bus_index, ...}`, `SetBusMute{bus_index, ...}`, `SetListener`, ...) — buses are addressed by the stable `uint32_t` index above, never by name, to keep the struct POD. No `std::function`, no heap payloads; pointers to already-loaded clips only. Clips are kept alive by the engine's `shared_ptr` cache for as long as any voice references them, and released only after the audio thread has confirmed the voice is free: the engine holds a `std::vector<std::shared_ptr<AudioClip>> pending_release_`, and an entry is only erased once `Freed` events have been observed for every voice that referenced it. Never erase a cache entry directly inside `stop()`.
- **`Event`** — POD on the `EventRing`: `{ EventType type; uint32_t voice_index; uint32_t generation; }` with `EventType ∈ { Finished, Looped, Freed, Stolen }`. `on_voice_finished` fires for `Finished` (natural end) and a completed `Stop`, but **not** for `Stolen` — a caller whose voice was stolen to make room for a higher-priority one should not see a phantom "finished" callback.

## Phase 5 — Spatialization

**`[REVALIDATED]` build-order note:** despite the phase numbering, `spatial/` is implemented **before** `data/import_settings.h` (Phase 2), because `ClipImportSettings::spatial` holds a `spatial::SpatialParams` **by value** and C++ can't forward-declare a by-value member of an incomplete type. Phase numbers here describe the *design's* dependency direction (spatialization conceptually builds on clips); the *build* sequence in this doc's companion implementation plan does dsp → spatial → wav_chunks → decoder → import_settings → audio_clip instead.

- **`spatial/listener.h`** — `struct Listener { glm::vec3 position{0.0f}; glm::vec3 velocity{0.0f}; glm::vec3 forward{0,0,-1}; glm::vec3 up{0,1,0}; glm::mat3 basis() const; }` plus `set_from_transform(const coopa::util::Transform&)` reading `get_world_matrix()` from `<coopa/util/transform.h>`. **`[REVALIDATED]`**: there is no `world_position()` accessor on `Transform` — extract it as `glm::vec3(transform.get_world_matrix()[3])` (column 3 of the 4×4). Also note `get_world_matrix()` is `const` but mutates a `mutable` cache while walking the parent chain, and libcoopa's own docs call out that concurrent reads/writes of that cache from separate threads are unsafe — `set_from_transform` must only ever be called on the main thread (inside `AudioListener::update(dt)` in the deferred Phase 9, or directly by the app before `update(dt)`), never from the audio callback.
- **`spatial/attenuation.h`** — `enum class RolloffMode { Inverse, Linear, Logarithmic, Custom }`, `struct AttenuationCurve { float min_distance = 1.0f, max_distance = 500.0f; RolloffMode mode = Inverse; float rolloff = 1.0f; std::function<float(float)> custom; float evaluate(float distance) const; }` (Inverse: `min / (min + rolloff * (d - min))`, clamped to 0 past `max_distance`).
- **`spatial/panner.h`** — the math the user asked to own:
  ```cpp
  struct SpatialParams {
      glm::vec3 position{0.0f};
      glm::vec3 velocity{0.0f};
      glm::vec3 direction{0,0,-1};
      float spatial_blend = 1.0f;        /**< 0 = pure 2D, 1 = fully 3D. */
      float spread = 0.0f;               /**< Widens the image at close range. */
      AttenuationCurve attenuation{};
      float inner_cone_angle = 360.0f, outer_cone_angle = 360.0f, outer_cone_gain = 1.0f;
      float doppler_factor = 1.0f;
      bool enable_itd = true, enable_head_shadow = true;
  };
  struct EarParams { float gain_l, gain_r; float delay_l, delay_r; float cutoff_l, cutoff_r; float pitch_scale; };
  class BinauralPanner {   // implements IPanner; an HrtfPanner can replace it later
      EarParams compute(const SpatialParams&, const Listener&, uint32_t sample_rate) const;
      void process(const float* mono_in, float* stereo_out, uint32_t frames);
  };
  ```
  Computation, in order:
  1. **Listener space:** `local = glm::transpose(listener.basis()) * (params.position - listener.position)`; `distance = length(local)`; `azimuth = atan2(local.x, -local.z)`; `elevation = asin(local.y / distance)`.
  2. **ITD (Woodworth):** `itd = (a/c) * (theta + sin theta)` for `|theta| <= pi/2`, else `(a/c) * (pi - |theta| + sin|theta|) * sign`, with `a = 0.0875 m`, `c = 343 m/s`. Max ≈ 0.66 ms ≈ 32 samples @ 48 kHz. The **contralateral** ear's `DelayLine` gets the delay; ipsilateral gets 0.
  3. **Head shadow:** per-ear `OnePoleLowpass` with `cutoff = k_shadow_max_hz * (0.5f + 0.5f * cos(angle_to_ear))` (~1.5 kHz fully shadowed → ~20 kHz facing), plus a broadband ILD gain of the same shape.
  4. **Distance:** `gain *= attenuation.evaluate(distance)`.
  5. **Cone:** smoothstep between `inner_cone_angle` and `outer_cone_angle` down to `outer_cone_gain`, using `params.direction`.
  6. **Doppler:** `pitch_scale = (c + dot(listener.velocity, dir)) / (c + dot(params.velocity, dir))`, lerped by `doppler_factor` and clamped to `[0.5, 2.0]`.
  7. **Blend/spread:** lerp the whole binaural result toward centered stereo by `1 - spatial_blend`; `spread` narrows ITD/ILD as distance approaches `min_distance` so walking through a source doesn't hard-pan.
  8. All outputs are *targets* handed to the voice's ramping, never applied instantaneously.

  **`[REVALIDATED]` panner cadence, settled:** `compute()` runs on the **main thread**, inside `AudioEngine::update(dt)`, for every active spatialized voice, and its `EarParams` result is pushed as a `SetVoiceSpatial` command whose fields the audio thread only ramps toward — it never calls `compute()` itself. This keeps `atan2`/`asin`/`sqrt` off the audio callback even at `k_max_voices` concurrent spatialized sources, at the cost of up to one `update(dt)` frame of positional lag, which is inaudible at typical frame rates and is exactly what "all outputs are targets" above already implies.

  **Mono requirement.** Both `ex/` files are stereo, which cannot be fed through ITD/ILD as-is — a per-ear delay/gain pair presumes a single point-source signal, and there is no principled way to binauralize an already-stereo bed (it's not "one direction," it's a pre-mixed field). `AudioClip::load` therefore **rejects** `spatialize = true` combined with a source of >1 channel unless `force_mono = true` also downmixes it first. **`[REVALIDATED]` downmix constant, settled: `(L + R) * 0.5f`, not equal-power `/√2`.** The two channels of a pre-mixed stereo bed (exactly the `ex/` fixtures' case) are correlated, not independent — summing correlated signals at the equal-*power* `1/√2` coefficient comes out roughly 3 dB hotter than the source, which is the wrong intuition for "downmix"; equal-*power* mixing is the right formula for *independent, decorrelated* signals (e.g. panning one mono voice), not for collapsing an already-stereo file. This is exactly why `ClipImportSettings::force_mono` exists as a field, not an afterthought — surfaced by the fixtures, not invented for them. `play_3d()` throws `std::runtime_error("[sfxcoopa] play_3d: clip '<path>' is stereo; set force_mono in its import settings")` if this is violated, since a silent auto-downmix would surprise a sound designer more than a clear error would.

## Phase 6 — Device and engine facade

- **`core/device.h`** — `class AudioDevice`: RAII over `ma_device` (`ma_device_init`/`start` in ctor, `uninit` in dtor, non-copyable), configured f32 / stereo / engine rate. Static callback forwards straight to `Mixer::render`. A `DeviceConfig { uint32_t sample_rate; uint32_t buffer_frames = 480; bool null_backend = false; }` — `null_backend` (also honoured via `SFX_DEVICE=null`) selects `ma_backend_null` so CI and headless boxes work.
- **`core/engine.h`** — `class AudioEngine`, the one object apps construct:
  ```cpp
  explicit AudioEngine(const DeviceConfig& config = {});
  VoiceHandle play(const std::string& clip_path, const PlayParams& params = {});
  VoiceHandle play_3d(const std::string& clip_path, const glm::vec3& position, const PlayParams& params = {});
  void stop(VoiceHandle); void pause(VoiceHandle); void set_voice_gain(VoiceHandle, float); ...
  MixerBus& master(); MixerBus& create_bus(const std::string& name, MixerBus& parent);
  MixerBus* find_bus(const std::string& name);
  void set_listener(const Listener&);
  void update(float delta_time);            /**< Main thread: drain events, emit signals, GC clips. */
  void render_offline(float* out, uint32_t frames);   /**< Hardware-free path for tests/bounces. */
  std::shared_ptr<AudioClip> load_clip(const std::string& path, const ClipImportSettings& = {});
  coopa::event::Signal<VoiceHandle> on_voice_finished;   /**< Emitted from update(), main thread. */
  coopa::event::Signal<VoiceHandle> on_voice_looped;
  ```
  Ctor-init/dtor-shutdown only — no `init()`/`shutdown()` methods, per house style. Logger member `coopa::debug::Logger("AudioEngine")`.

  **`[REVALIDATED]` clip cache key, settled:** plain `std::unordered_map<std::string, std::shared_ptr<AudioClip>>` keyed by path alone is wrong — `load_clip("ex/x.wav", {.force_mono=true})` and `load_clip("ex/x.wav", {.force_mono=false})` are different decoded PCM buffers that would otherwise collide on the same cache entry. Key on `path + ":" + hash-of-load-affecting-settings`, where the hash covers only the fields that change the decoded bytes (`load_type`, `force_mono`, `sample_rate_override`) — not `volume`/`pitch`/`spatial`/etc., which are applied at play time, not bake time. Main-thread only; no `ParallelMap` needed since the audio thread never touches it, consistent with the original design.

## Phase 7 — Streaming

- **`data/audio_stream.h`** — `class AudioStream`: owns a `Decoder`, a `RingBuffer<float>` (~1 s of PCM), and loop points. `pump()` (decode thread) tops the ring up; `read(float* out, uint32_t frames)` (audio thread) drains it and reports underruns by filling silence and incrementing an atomic counter rather than blocking.
- A single `StreamManager` owns one `coopa::job::Thread` (from `<coopa/job/thread.h>`, which supplies the cooperative stop flag and lifecycle logging) that round-robins `pump()` across all active streams every few ms. Selected by `LoadType::Streaming` in `ClipImportSettings`, so choosing streaming vs. fully-decoded is an *import* decision, not a call-site one.

## Phase 8 — Effects

`dsp/effects/`: `Lowpass`/`Highpass` (Biquad wrappers — lowpass doubles as the occlusion knob), `Delay` (feedback + wet/dry), `Reverb` (Freeverb-style: 8 parallel comb + 4 series allpass per channel, with stereo spread — compact, no FFT), `Limiter` (lookahead brickwall for the master bus). All implement `IEffect`, all pre-allocate in `prepare()`.

## Phase 9 — Scene integration

- **`components/audio_source.h`** — `class AudioSource : public coopa::scene::Component`, `type_name()` → `"AudioSource"`. Public config: `clip_path`, `bus`, `volume`, `pitch`, `loop`, `play_on_start`, `spatialize`, `SpatialParams spatial`. `start()` optionally plays; `update(dt)` pushes the owner's world position/velocity (derived from `Transform::get_world_matrix()`, velocity = delta/dt) to the engine. Exposes `play()`, `stop()`, `pause()`, and `coopa::event::Signal<> on_finished` — **reusing** `coopa::event::Signal`, never defining a new event primitive (uicoopa's stated rule).
- **`components/audio_listener.h`** — pushes the owner transform into `AudioEngine::set_listener()` each `update(dt)`. A one-line hookup to a gfxcoopa camera object app-side.
- **Status (2026-10-05): implemented.** `components/audio_source.h`, `components/audio_listener.h`, `components/sfx_resources.h` (SfxResources lives in its own header so the components can use it; `sfx_yaml.h` includes it) and `sfx_yaml.h`, with 13 headless tests. Engine additions to support it: `PlayParams::loop`/`spatial` overrides, `set_voice_motion()`, `is_voice_active()`, `format()`, generation-checked Finished/Stolen bookkeeping, and `VoicePool::mark_pending()` so a slot whose Play command hasn't been rendered yet isn't reclaimed by an intervening `update()`. Completion is polled (`is_voice_active`) rather than subscribed, so a destroyed component never leaves a dangling slot on `on_voice_finished`.
- **`sfx_yaml.h`** — `inline void register_sfx_components()` registering `"!AudioSource"` / `"!AudioListener"` parsers with `coopa::scene::SceneLoader::register_component_parser`, plus a `SfxResources` singleton (`instance().set_engine(AudioEngine*)`) mirroring `uicoopa/ui_yaml.h`'s `UIResources`, since parser callbacks get no engine handle. One-directional, opt-in — exactly the uicoopa bridge shape.

## Phase 10 — Documentation

Per-module `README.md` in each of `util/ collections/ data/ dsp/ spatial/ mixer/ core/ components/`. Root `README.md` with quick start, module table and the audio-thread contract stated up front. Then `coopadocs build` must be warning-free.

**`[REVALIDATED]` format, corrected:** the mermaid `## Architecture & Design` / numbered `### Components` / `## Usage Code Example` format originally specified here exists in exactly **one** file in the whole workspace (`libcoopa/coopa/job/README.md`) — it is not the house standard. The actual standard, checked into `gfxcoopa/.agents/skills/add-module-readme/SKILL.md` and followed by all 8 of gfxcoopa's module READMEs (uicoopa has none to compare against), is: `# <Name> Submodule (`coopa::sfx::x`)` → 1–2 paragraph summary → `## <Name> Architecture` (or `Pipeline Architecture`) with an ASCII box-and-arrow diagram in a ` ```text ` block using unicode box-drawing characters → `## File Breakdown` with one `### [filename.h](file:///absolute/path/to/filename.h)` entry per header, each listing Role / Key Classes / Details → `## Usage Example`. Use this format, not the mermaid one, when Phase 10 is picked back up.

---

## Testing and Validation

### Headless suite — `test.cpp`

Hand-rolled test harness (`RUN_TEST`, `ASSERT_TRUE`, `ASSERT_EQ`, `ASSERT_NEAR`), plus a new `ASSERT_VEC3_NEAR`. **`[REVALIDATED]`**: no single sibling file has this whole set — `uicoopa/test.cpp` has `RUN_TEST`/`ASSERT_TRUE`/`ASSERT_NEAR`/`ASSERT_VEC2_NEAR` but no `ASSERT_EQ`; `ASSERT_EQ` exists only in `gfxcoopa/test.cpp`. Take the union of both files' macro blocks rather than copying either one alone. **Opens no audio device** — everything goes through `render_offline()`. Tests grouped by `// --- <module>/<file>.h ---` banners:

| Area | Assertions |
|---|---|
| `ring_buffer.h` | SPSC stress: producer/consumer `std::thread` pair moving 1M ints, zero loss, zero duplication; wrap-around correctness; `available_read/write` invariants |
| `decoder.h` / `audio_clip.h` | `ex/SEFE_Whoosh03.wav` decodes to exactly 31 903 frames / 44 100 Hz / 2 ch; `ex/SEFE_Flashback.wav` to 156 468 frames / 3.548 027 s. At an engine rate of 44 100, the fast-path load is **bit-exact** against the file's raw `data` chunk (memcmp, not `ASSERT_NEAR`). At 48 000, frame count matches `31903 * 48000/44100` within ±1. `force_mono` on either file halves the sample count and forbids `spatialize` unless set. Decode the added mp3 fixture: duration within one MPEG granule (1152 frames) of the wav it was encoded from, waveform cross-correlated rather than sample-compared |
| `wav_chunks.h` | `read_smpl_loops` on both `ex/` files returns an empty vector (no loop points authored) without throwing on their leading `JUNK` chunk; a synthetic wav with a `JUNK` chunk **and** a `smpl` chunk with one loop round-trips the exact start/end frame |
| `import_settings.h` | Sidecar round-trip (`save` → `load` → field equality); missing sidecar yields defaults, not a throw; explicit arg overrides sidecar overrides embedded `smpl` loop points |
| `dsp/` | One-pole DC gain = 1; biquad lowpass attenuates a 10 kHz sine and passes a 100 Hz one; `DelayLine` fractional read of an impulse lands the expected energy split across two samples; `Resampler` at ratio 2.0 yields 2× frames and preserves a sine's frequency |
| `mixer/bus.h` | Bus at volume 0.5 halves amplitude; mute silences; solo on a sibling silences this bus; nested bus gains multiply; effect chain applies in order |
| `mixer/voice.h` | `VoicePool` exhaustion steals rather than throws; a released handle's `generation` bump makes the stale handle a no-op; loop points wrap at the exact frame; stop fades rather than cuts (no sample-to-sample delta > threshold) |
| `spatial/panner.h` | Source hard right → right ear louder **and** left ear delayed by ≈32 samples @48 kHz (cross-correlation peak); source dead ahead → `gain_l == gain_r`, both delays 0; source behind vs. in front → equal ITD (documented front/back ambiguity, asserted deliberately); attenuation at `min_distance` = 1.0, past `max_distance` = 0.0; approaching source → `pitch_scale > 1` |
| `core/engine.h` | `render_offline` of one clip on the master bus reproduces the clip's samples within 1e-5; two voices sum; `on_voice_finished` fires exactly once after `update(dt)` following playback end |
| `audio_stream.h` | Stream a long file fully offline; PCM matches the `DecompressOnLoad` decode of the same file; underrun counter stays 0 |

Run: `cbuild` then `ctest --test-dir build --output-on-failure`, or `./build/sfxcoopa` directly. Fallback: `cmake -B build && cmake --build build`.

### Audible demo — `test_playback.cpp`

Separate target so `ctest` never needs a sound card. Opens a real device, creates `Music` and `SFX` buses under master at different levels, streams `ex/SEFE_Flashback.wav` (3.5 s, long enough to be a believable streaming case) on `Music`, and orbits `ex/SEFE_Whoosh03.wav` (force-mono, one-shot) around the listener on `SFX` so ITD/ILD is audible on headphones. Env-gated: `MAX_SECONDS=n` to auto-exit, `SFX_DEVICE=null` to run silently, `BOUNCE=out.wav` to write the master to `output/` instead of playing it (via `ma_encoder`, confirmed present in the vendored miniaudio and deliberately not trimmed by the `MA_NO_*` set).

**`[REVALIDATED]`**: `MAX_SECONDS` is **not** an existing house convention — the established pattern (`gfxcoopa/gfxcoopa/app/context.h`'s `ContextConfig::from_env()`) is `MAX_FRAMES=<n>` and `ONESHOT=1`, both frame-counted, which don't map cleanly onto "play N seconds of audio." Keep `MAX_SECONDS` here as a deliberate, new, audio-specific variable (frames aren't the natural unit for a listener-orbit demo), but also honour `ONESHOT` (render exactly one device callback then exit) for consistency with every other demo in the workspace.

```bash
cbuild
MAX_SECONDS=10 ./build/sfxcoopa_playback          # audible orbit test
BOUNCE=orbit.wav ./build/sfxcoopa_playback        # offline bounce for inspection
```

### Manual verification

1. **Headphone check** — the orbiting source must sweep continuously ear to ear with no clicks at the crossover, and get quieter/duller as it passes behind.
2. **Bounce inspection** — open `output/orbit.wav` in a wave editor and confirm the L/R channels are phase-offset (ITD present) and the amplitude envelopes alternate (ILD present).
3. **Level independence** — `find_bus("Music")->set_volume_db(-12.0f)` at runtime changes only the music, and ramps rather than steps.
4. **CPU sanity** — 64 concurrent spatialized voices at 48 kHz / 480-frame buffers with no underruns (miniaudio reports them); log the mixer's per-callback time under `#ifdef SFXCOOPA_DIAGNOSTICS`, mirroring libcoopa's `COOPA_JOB_DIAGNOSTICS` pattern.
5. **Docs** — `coopadocs build` reports no warnings; `coopadocs show` renders every module.
6. **Downstream** — add `set(SFXCOOPA_DIR "${ROOT_DIR_PARENT}/sfxcoopa")` + `add_subdirectory`/`target_link_libraries(blendy PRIVATE coopa::sfx)` (not bare `include_directories`, per the Phase 0 `[REVALIDATED]` note — miniaudio needs a linked implementation) to `/home/coopa/git/blendy/CMakeLists.txt` and confirm blendy still builds with an `AudioListener` on its camera.

## 2026-08-29 build pass — scope

This revalidation pass implements **Phases 0–6** (scaffolding through the offline-capable engine facade plus the device/demo target), with the headless suite green at every checkpoint. **Phases 7–10 — streaming (`audio_stream.h`/`StreamManager`), the `dsp/effects/` chain, scene integration (`components/`, `sfx_yaml.h`), and module READMEs — are deferred**, not abandoned: the Phase 4 `SourceRef` seam and the Phase 10 README-format correction above are both applied now specifically so that follow-up work doesn't have to undo anything. `blendy` is left unmodified in this pass; item 6 above is recorded for when it's picked up.

**Status (2026-10-07):** Phase 9 (scene integration) has since been implemented — see its status note above. Phases 7 (streaming), 8 (`dsp/effects/`) and the per-module READMEs of Phase 10 remain unbuilt; the root `README.md` exists.
