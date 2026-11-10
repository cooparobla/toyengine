/**
 * @file sfx_yaml.h
 * @brief Registers sfxcoopa's scene components (AudioSource, AudioListener)
 * with coopa::scene::SceneLoader, and re-exports SfxResources.
 *
 * The one-directional bridge, mirroring uicoopa/ui_yaml.h: libcoopa has no
 * dependency on sfxcoopa; an application that wants audio components loadable
 * from scene YAML includes this header and calls register_sfx_components()
 * once at startup. Names are registered bare ("AudioSource"); SceneLoader
 * normalizes both a "!AudioSource" tag and a "type: AudioSource" key to the
 * same name (prefer "type:", see scene_loader.h's fkYAML note).
 *
 * Parser callbacks get no engine handle, so components reach the engine
 * through SfxResources::instance() -- call set_engine() before Scene::start().
 *
 * @code
 * - type: AudioSource
 *   clip: sounds/engine_loop.wav   # relative: scene dir / prefab dir, then SfxResources roots
 *   bus: SFX                       # default SFX; Master if the bus doesn't exist
 *   volume: 0.8
 *   pitch: 1.0
 *   loop: true
 *   play_on_start: true
 *   spatialize: true               # mono clips only (force_mono in the .import sidecar)
 *   min_distance: 1.0
 *   max_distance: 40.0
 *   curve: Inverse                 # Inverse | Linear | Logarithmic
 *   rolloff: 1.0
 *   spatial_blend: 1.0
 *   spread: 0.0
 *   doppler: 1.0
 *   priority: 0
 *
 * - type: AudioListener
 *   track_velocity: true
 * @endcode
 */

#ifndef SFXCOOPA_SFX_YAML_H
#define SFXCOOPA_SFX_YAML_H

#include <sfxcoopa/components/audio_listener.h>
#include <sfxcoopa/components/audio_source.h>
#include <sfxcoopa/components/sfx_resources.h>
#include <sfxcoopa/data/import_settings.h>

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <fkYAML/node.hpp>

#include <filesystem>
#include <string>

namespace coopa {
namespace sfx {

namespace detail {

inline float yaml_float(const fkyaml::node& node, const char* key, float fallback) {
    if (!node.contains(key)) return fallback;
    const auto& v = node.at(key);
    if (v.is_float_number()) return static_cast<float>(v.get_value<double>());
    if (v.is_integer()) return static_cast<float>(v.get_value<int64_t>());
    return fallback;
}

inline bool yaml_bool(const fkyaml::node& node, const char* key, bool fallback) {
    if (!node.contains(key)) return fallback;
    const auto& v = node.at(key);
    return v.is_boolean() ? v.get_value<bool>() : fallback;
}

inline std::string yaml_string(const fkyaml::node& node, const char* key, const std::string& fallback) {
    if (!node.contains(key)) return fallback;
    const auto& v = node.at(key);
    return v.is_string() ? v.get_value<std::string>() : fallback;
}

}  // namespace detail

/**
 * @brief Parses AudioSource fields from a YAML mapping onto `src`. Exposed for
 * hosts (e.g. an editor inspector or prefab instancer) that build components
 * outside SceneLoader. `ctx` may be nullptr; when given, a relative clip path
 * is resolved against the declaring file's directory first.
 */
inline void parse_audio_source(const fkyaml::node& node, components::AudioSource& src,
                               const coopa::scene::SceneLoader::ParseContext* ctx = nullptr) {
    using detail::yaml_bool;
    using detail::yaml_float;
    using detail::yaml_string;

    std::string clip = yaml_string(node, "clip", yaml_string(node, "clip_path", src.clip_path));
    if (ctx && !clip.empty() && !std::filesystem::path(clip).is_absolute()) {
        // ParseContext::resolve returns the input unchanged when no candidate
        // exists, leaving SfxResources' roots to resolve it at play() time.
        clip = ctx->resolve(clip);
    }
    src.clip_path = clip;
    src.bus = yaml_string(node, "bus", src.bus);
    src.volume = yaml_float(node, "volume", src.volume);
    src.pitch = yaml_float(node, "pitch", src.pitch);
    src.loop = yaml_bool(node, "loop", src.loop);
    src.play_on_start = yaml_bool(node, "play_on_start", src.play_on_start);
    src.spatialize = yaml_bool(node, "spatialize", src.spatialize);
    if (node.contains("priority") && node.at("priority").is_integer()) {
        src.priority = static_cast<int>(node.at("priority").get_value<int64_t>());
    }

    auto& sp = src.spatial;
    sp.attenuation.min_distance = yaml_float(node, "min_distance", sp.attenuation.min_distance);
    sp.attenuation.max_distance = yaml_float(node, "max_distance", sp.attenuation.max_distance);
    sp.attenuation.rolloff = yaml_float(node, "rolloff", sp.attenuation.rolloff);
    if (node.contains("curve")) {
        const std::string curve = yaml_string(node, "curve", "Inverse");
        // Custom needs a callable and can't come from YAML; treat it as Inverse.
        sp.attenuation.mode = (curve == "Custom") ? coopa::sfx::spatial::RolloffMode::Inverse
                                                  : coopa::sfx::data::rolloff_mode_from_string(curve);
    }
    sp.spatial_blend = yaml_float(node, "spatial_blend", sp.spatial_blend);
    sp.spread = yaml_float(node, "spread", sp.spread);
    sp.doppler_factor = yaml_float(node, "doppler", sp.doppler_factor);
    sp.inner_cone_angle = yaml_float(node, "inner_cone_angle", sp.inner_cone_angle);
    sp.outer_cone_angle = yaml_float(node, "outer_cone_angle", sp.outer_cone_angle);
    sp.outer_cone_gain = yaml_float(node, "outer_cone_gain", sp.outer_cone_gain);
}

/** @brief Registers "AudioSource" and "AudioListener" (tag or type: form) with SceneLoader. */
inline void register_sfx_components() {
    using coopa::scene::SceneLoader;
    using coopa::scene::SceneObject;

    SceneLoader::register_component_parser(
        "AudioSource", [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* src = obj.add_component<components::AudioSource>();
            parse_audio_source(node, *src, &ctx);
        });

    SceneLoader::register_component_parser(
        "AudioListener", [](const fkyaml::node& node, SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* listener = obj.add_component<components::AudioListener>();
            listener->track_velocity = detail::yaml_bool(node, "track_velocity", listener->track_velocity);
        });
}

}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_SFX_YAML_H
