/**
 * @file import_settings.h
 * @brief Per-clip import configuration, with a ".import" YAML sidecar.
 */

#ifndef SFXCOOPA_DATA_IMPORT_SETTINGS_H
#define SFXCOOPA_DATA_IMPORT_SETTINGS_H

#include <sfxcoopa/spatial/panner.h>
#include <sfxcoopa/data/wav_chunks.h>

#include <coopa/collections/yaml_map.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace coopa {
namespace sfx {
namespace data {

/** @brief Whether a clip is fully decoded up front or streamed from disk during playback. */
enum class LoadType {
    DecompressOnLoad,
    Streaming
};

inline std::string load_type_to_string(LoadType t) {
    return t == LoadType::Streaming ? "Streaming" : "DecompressOnLoad";
}

inline LoadType load_type_from_string(const std::string& s) {
    return s == "Streaming" ? LoadType::Streaming : LoadType::DecompressOnLoad;
}

inline std::string rolloff_mode_to_string(coopa::sfx::spatial::RolloffMode m) {
    using coopa::sfx::spatial::RolloffMode;
    switch (m) {
        case RolloffMode::Linear: return "Linear";
        case RolloffMode::Logarithmic: return "Logarithmic";
        case RolloffMode::Custom: return "Custom";
        case RolloffMode::Inverse: default: return "Inverse";
    }
}

inline coopa::sfx::spatial::RolloffMode rolloff_mode_from_string(const std::string& s) {
    using coopa::sfx::spatial::RolloffMode;
    if (s == "Linear") return RolloffMode::Linear;
    if (s == "Logarithmic") return RolloffMode::Logarithmic;
    if (s == "Custom") return RolloffMode::Custom;
    return RolloffMode::Inverse;
}

/**
 * @struct ClipImportSettings
 * @brief How one clip should be loaded, mixed, and (optionally) spatialized.
 *
 * Resolution order when loading a clip: explicit argument (a caller passing
 * its own ClipImportSettings to AudioClip::load(), bypassing load() below
 * entirely) > `.import` sidecar > embedded `smpl` loop points > these
 * struct defaults. load() itself implements the latter three tiers, using
 * YAMLMap::get(key, current_value)'s built-in fallback to flow smpl-derived
 * defaults through untouched when the sidecar doesn't mention a given key.
 *
 * The sidecar uses flat, prefixed keys (`spatial_min_distance`, not a
 * nested `spatial:` block) because YAMLMap::get_node() returns nested
 * values by value -- writes into a detached copy never propagate back to
 * the parent, which makes round-tripping a nested block error-prone. Flat
 * keys sidestep that entirely.
 */
struct ClipImportSettings {
    LoadType load_type = LoadType::DecompressOnLoad;
    bool force_mono = false;           /**< Downmix to 1 channel at import. */
    uint32_t sample_rate_override = 0; /**< 0 = keep source rate. */
    float volume = 1.0f;               /**< Gain multiplier; applied by AudioSource, not by AudioEngine::play(). */
    float pitch = 1.0f;                /**< Pitch multiplier; applied by AudioSource, not by AudioEngine::play(). */
    bool loop = false;
    uint64_t loop_start_frame = 0;
    uint64_t loop_end_frame = 0; /**< 0 = end of clip. */
    std::string default_bus = "Master";
    int priority = 0; /**< Saved/loaded only; play() takes priority from PlayParams. */
    bool spatialize = false;
    coopa::sfx::spatial::SpatialParams spatial{}; /**< Defaults used when spatialize is true. */

    /**
     * @brief Loads settings for a clip, merging (highest to lowest
     * precedence) its `.import` sidecar, its embedded `smpl` loop points
     * (if any), and struct defaults. Neither a missing sidecar nor a
     * missing/absent `smpl` chunk is an error.
     */
    static ClipImportSettings load(const std::string& clip_path) {
        ClipImportSettings settings;

        // Tier 3: embedded smpl loop points, if the source is a wav with
        // any authored. Non-wav sources (mp3) or wavs with no smpl chunk
        // simply leave the struct defaults in place -- not an error.
        try {
            std::vector<coopa::sfx::data::SampleLoop> loops = read_smpl_loops(clip_path);
            if (!loops.empty()) {
                settings.loop = true;
                settings.loop_start_frame = loops[0].start;
                settings.loop_end_frame = loops[0].end;
            }
        } catch (const std::exception&) {
            // Not a parseable RIFF/WAVE file -- no smpl-derived defaults available.
        }

        // Tier 2: the .import sidecar. It is optional, so a clip without one
        // skips the load (YAMLMap::load() reports a missing file on stderr);
        // get(key, current) preserves whatever tier 3/4 already set for every
        // key the sidecar doesn't mention.
        const std::string sidecar = clip_path + ".import";
        std::error_code sidecar_ec;
        if (!std::filesystem::exists(coopa::yaml::resolve_variant(sidecar), sidecar_ec)) return settings;
        coopa::collections::YAMLMap map = coopa::collections::YAMLMap::load(sidecar);

        settings.load_type = load_type_from_string(map.get<std::string>("load_type", load_type_to_string(settings.load_type)));
        settings.force_mono = map.get<bool>("force_mono", settings.force_mono);
        settings.sample_rate_override = map.get<uint32_t>("sample_rate_override", settings.sample_rate_override);
        settings.volume = map.get<float>("volume", settings.volume);
        settings.pitch = map.get<float>("pitch", settings.pitch);
        settings.loop = map.get<bool>("loop", settings.loop);
        settings.loop_start_frame = map.get<uint64_t>("loop_start_frame", settings.loop_start_frame);
        settings.loop_end_frame = map.get<uint64_t>("loop_end_frame", settings.loop_end_frame);
        settings.default_bus = map.get<std::string>("default_bus", settings.default_bus);
        settings.priority = map.get<int>("priority", settings.priority);
        settings.spatialize = map.get<bool>("spatialize", settings.spatialize);

        auto& sp = settings.spatial;
        sp.position.x = map.get<float>("spatial_position_x", sp.position.x);
        sp.position.y = map.get<float>("spatial_position_y", sp.position.y);
        sp.position.z = map.get<float>("spatial_position_z", sp.position.z);
        sp.velocity.x = map.get<float>("spatial_velocity_x", sp.velocity.x);
        sp.velocity.y = map.get<float>("spatial_velocity_y", sp.velocity.y);
        sp.velocity.z = map.get<float>("spatial_velocity_z", sp.velocity.z);
        sp.direction.x = map.get<float>("spatial_direction_x", sp.direction.x);
        sp.direction.y = map.get<float>("spatial_direction_y", sp.direction.y);
        sp.direction.z = map.get<float>("spatial_direction_z", sp.direction.z);
        sp.spatial_blend = map.get<float>("spatial_blend", sp.spatial_blend);
        sp.spread = map.get<float>("spatial_spread", sp.spread);
        sp.attenuation.min_distance = map.get<float>("spatial_attenuation_min_distance", sp.attenuation.min_distance);
        sp.attenuation.max_distance = map.get<float>("spatial_attenuation_max_distance", sp.attenuation.max_distance);
        sp.attenuation.rolloff = map.get<float>("spatial_attenuation_rolloff", sp.attenuation.rolloff);
        sp.attenuation.mode = rolloff_mode_from_string(
            map.get<std::string>("spatial_attenuation_mode", rolloff_mode_to_string(sp.attenuation.mode)));
        sp.inner_cone_angle = map.get<float>("spatial_inner_cone_angle", sp.inner_cone_angle);
        sp.outer_cone_angle = map.get<float>("spatial_outer_cone_angle", sp.outer_cone_angle);
        sp.outer_cone_gain = map.get<float>("spatial_outer_cone_gain", sp.outer_cone_gain);
        sp.doppler_factor = map.get<float>("spatial_doppler_factor", sp.doppler_factor);
        sp.enable_itd = map.get<bool>("spatial_enable_itd", sp.enable_itd);
        sp.enable_head_shadow = map.get<bool>("spatial_enable_head_shadow", sp.enable_head_shadow);

        return settings;
    }

    /** @brief Writes this settings struct as a "<clip_path>.import" YAML sidecar. */
    void save(const std::string& clip_path) const {
        coopa::collections::YAMLMap map;
        map.set("load_type", load_type_to_string(load_type));
        map.set("force_mono", force_mono);
        map.set("sample_rate_override", sample_rate_override);
        map.set("volume", volume);
        map.set("pitch", pitch);
        map.set("loop", loop);
        map.set("loop_start_frame", loop_start_frame);
        map.set("loop_end_frame", loop_end_frame);
        map.set("default_bus", default_bus);
        map.set("priority", priority);
        map.set("spatialize", spatialize);

        const auto& sp = spatial;
        map.set("spatial_position_x", sp.position.x);
        map.set("spatial_position_y", sp.position.y);
        map.set("spatial_position_z", sp.position.z);
        map.set("spatial_velocity_x", sp.velocity.x);
        map.set("spatial_velocity_y", sp.velocity.y);
        map.set("spatial_velocity_z", sp.velocity.z);
        map.set("spatial_direction_x", sp.direction.x);
        map.set("spatial_direction_y", sp.direction.y);
        map.set("spatial_direction_z", sp.direction.z);
        map.set("spatial_blend", sp.spatial_blend);
        map.set("spatial_spread", sp.spread);
        map.set("spatial_attenuation_min_distance", sp.attenuation.min_distance);
        map.set("spatial_attenuation_max_distance", sp.attenuation.max_distance);
        map.set("spatial_attenuation_rolloff", sp.attenuation.rolloff);
        map.set("spatial_attenuation_mode", rolloff_mode_to_string(sp.attenuation.mode));
        map.set("spatial_inner_cone_angle", sp.inner_cone_angle);
        map.set("spatial_outer_cone_angle", sp.outer_cone_angle);
        map.set("spatial_outer_cone_gain", sp.outer_cone_gain);
        map.set("spatial_doppler_factor", sp.doppler_factor);
        map.set("spatial_enable_itd", sp.enable_itd);
        map.set("spatial_enable_head_shadow", sp.enable_head_shadow);

        map.save(clip_path + ".import");
    }
};

}  // namespace data
}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_DATA_IMPORT_SETTINGS_H
