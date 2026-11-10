#pragma once

/**
 * @file scene_audio.h
 * @brief Fixtures for the scene-component suites (AudioSource, AudioListener): a scoped
 *        SfxResources binding, scene-from-YAML loading, and offline-render RMS measurement.
 */

#include "clip_fixtures.h"

#include <sfxcoopa/components/audio_listener.h>
#include <sfxcoopa/components/audio_source.h>
#include <sfxcoopa/core/engine.h>
#include <sfxcoopa/data/import_settings.h>
#include <sfxcoopa/sfx_yaml.h>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <fkYAML/node.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace sfx_test {

using coopa::sfx::SfxResources;
using coopa::sfx::components::AudioListener;
using coopa::sfx::components::AudioSource;
using coopa::sfx::core::AudioEngine;
using coopa::sfx::data::AudioFormat;

/**
 * @brief Binds the SfxResources singleton to `engine` for one test and restores a clean
 *        singleton afterwards (no engine, no search roots, no pending listener push), so no
 *        test sees another's state through it.
 */
struct ResourcesScope {
    explicit ResourcesScope(AudioEngine& engine) {
        coopa::sfx::register_sfx_components();
        SfxResources::instance().set_engine(&engine);
        SfxResources::instance().clear_search_roots();
        SfxResources::instance().consume_listener_pushed();
    }
    ~ResourcesScope() {
        SfxResources::instance().set_engine(nullptr);
        SfxResources::instance().clear_search_roots();
        SfxResources::instance().consume_listener_pushed();
    }
    ResourcesScope(const ResourcesScope&)            = delete;
    ResourcesScope& operator=(const ResourcesScope&) = delete;
};

/**
 * @brief A force_mono copy of the whoosh (with a `.import` sidecar carrying `sidecar_volume`)
 *        in scratch; returns its path. Spatialized sources need a mono clip.
 */
inline std::string mono_whoosh_copy(const std::string& name = "mono_whoosh.wav", float sidecar_volume = 1.0f) {
    const std::string path = fixture_copy(k_whoosh, name);
    coopa::sfx::data::ClipImportSettings settings;
    settings.force_mono = true;
    settings.volume = sidecar_volume;
    settings.save(path);
    return path;
}

/**
 * @brief Builds a Scene from YAML text (as if loaded from `scene_path`, default a scratch
 *        path) and runs its first frame -- AudioSource's play_on_start fires on its first
 *        update(), not at load.
 */
inline coopa::scene::Scene load_scene(const std::string& yaml, const std::string& scene_path = "") {
    fkyaml::node root = fkyaml::node::deserialize(yaml);
    const std::string path = scene_path.empty()
        ? (coopa::test::scratch_dir("scene") / "scene.yaml").string()
        : scene_path;
    coopa::scene::Scene scene = coopa::scene::SceneLoader::load_from_node(root, path);
    scene.update(0.0f);
    return scene;
}

/** @brief One object with an AudioSource (extra keys appended verbatim) and an optional x position. */
inline std::string source_scene_yaml(const std::string& clip, const std::string& extra, float x = 0.0f) {
    return "scene:\n"
           "  scene_name: AudioTest\n"
           "  root_objects:\n"
           "    - name: Speaker\n"
           "      components:\n"
           "        - type: Transform\n"
           "          position: { x: " + std::to_string(x) + ", y: 0.0, z: 0.0 }\n"
           "        - type: AudioSource\n"
           "          clip: \"" + clip + "\"\n" + extra;
}

/** @brief Renders `frames` stereo frames offline and returns each channel's RMS. */
inline void render_rms(AudioEngine& engine, uint32_t frames, float& rms_l, float& rms_r) {
    std::vector<float> out(static_cast<size_t>(frames) * 2, 0.0f);
    engine.render_offline(out.data(), frames);
    double sl = 0.0, sr = 0.0;
    for (uint32_t i = 0; i < frames; ++i) {
        sl += double(out[2 * i]) * out[2 * i];
        sr += double(out[2 * i + 1]) * out[2 * i + 1];
    }
    rms_l = static_cast<float>(std::sqrt(sl / frames));
    rms_r = static_cast<float>(std::sqrt(sr / frames));
}

/** @brief Renders `frames` stereo frames offline and returns the mean of both channels' RMS. */
inline float render_rms(AudioEngine& engine, uint32_t frames) {
    float l = 0.0f, r = 0.0f;
    render_rms(engine, frames, l, r);
    return 0.5f * (l + r);
}

}  // namespace sfx_test
