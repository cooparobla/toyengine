/**
 * @file sound_library.h
 * @brief Process-wide registry of named sound effects, loaded from a YAML manifest.
 *
 * Deliberately mirrors ThemeLibrary (uicoopa/builder/ui_theme_yaml.h) and
 * IconLibrary (uicoopa/render/icon_library.h): a search-dir-relative
 * name -> data lookup with a singleton instance() and a clear() applications
 * call at shutdown. SoundLibrary itself never touches sfxcoopa or opens a
 * device -- it only resolves names to file paths and manifest metadata
 * (gain, pitch_jitter, category), so it has zero dependency on
 * UICOOPA_HAS_AUDIO and can be unit-tested with no audio backend at all.
 * UiAudio (ui_audio.h) is what actually plays a resolved path.
 */

#ifndef UICOOPA_AUDIO_SOUND_LIBRARY_H
#define UICOOPA_AUDIO_SOUND_LIBRARY_H

#include <fkYAML/node.hpp>
#include <coopa/yaml/document.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace coopa {
namespace ui {

/**
 * @struct SoundDef
 * @brief One manifest entry: a resolved file path plus per-sound playback defaults.
 */
struct SoundDef {
    std::string name;
    std::string path;            /**< Resolved as `<search_dir>/<file>` at load_manifest() time. */
    std::string category;        /**< Free-form grouping (e.g. "ui", "feedback"); not interpreted here. */
    float gain = 1.0f;           /**< Multiplied into PlayParams::gain by UiAudio::play(). */
    float pitch_jitter = 0.0f;   /**< +/- fraction applied to PlayParams::pitch by UiAudio::play(). */
};

/**
 * @class SoundLibrary
 * @brief Loads a `sounds.yaml` manifest (see sfxcoopa/tools/bake_ui_sfx.cpp for the shape it
 * writes) and publishes each entry as a name -> SoundDef lookup.
 *
 * @code
 * SoundLibrary::instance().set_search_dir(ROOT_DIR "/assets/sounds");
 * SoundLibrary::instance().load_manifest("sounds.yaml");
 * const SoundDef* click = SoundLibrary::instance().find("ui_click_soft");
 * @endcode
 */
class SoundLibrary {
public:
    static SoundLibrary& instance() {
        static SoundLibrary library;
        return library;
    }

    /** @brief Directory `load_manifest()` resolves both the manifest and each entry's `file` against. */
    void set_search_dir(const std::string& dir) { search_dir_ = dir; }

    /**
     * @brief Parses `<search_dir>/manifest_name` and publishes every entry.
     *
     * A missing or unparseable manifest is reported to stderr and leaves the registry
     * untouched (mirroring IconLibrary::add_sheet()'s "never fatal" contract) rather than
     * throwing -- a UI with no sound effects should still run.
     * @return True if the manifest was parsed and at least loaded (even zero entries is success).
     */
    bool load_manifest(const std::string& manifest_name = "sounds.yaml") {
        const std::string manifest_path = search_dir_ + "/" + manifest_name;
        const std::filesystem::path resolved = coopa::yaml::resolve_variant(manifest_path);
        if (!std::filesystem::exists(resolved)) {
            std::cerr << "[uicoopa] SoundLibrary: failed to open manifest '" << manifest_path << "'\n";
            return false;
        }

        try {
            fkyaml::node root = coopa::yaml::load_document(resolved);
            if (!root.contains("sounds")) return true; // empty manifest is not an error
            for (const auto& entry : root.at("sounds")) {
                SoundDef def;
                def.name = entry.at("name").get_value<std::string>();
                const std::string file_name = entry.contains("file") ? entry.at("file").get_value<std::string>()
                                                                       : def.name + ".wav";
                def.path = search_dir_ + "/" + file_name;
                def.category = entry.contains("category") ? entry.at("category").get_value<std::string>() : "";
                def.gain = entry.contains("gain") ? entry.at("gain").get_value<float>() : 1.0f;
                def.pitch_jitter = entry.contains("pitch_jitter") ? entry.at("pitch_jitter").get_value<float>() : 0.0f;
                sounds_[def.name] = def;
            }
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[uicoopa] SoundLibrary: failed to parse manifest '" << manifest_path << "': " << e.what()
                       << "\n";
            return false;
        }
    }

    /** @brief Looks up a published sound. @return nullptr if `name` was never loaded. */
    const SoundDef* find(const std::string& name) const {
        auto it = sounds_.find(name);
        return it != sounds_.end() ? &it->second : nullptr;
    }

    /** @brief All published names whose category exactly matches (case-sensitive); unsorted. */
    std::vector<std::string> names_in_category(const std::string& category) const {
        std::vector<std::string> names;
        for (const auto& [name, def] : sounds_) {
            if (def.category == category) names.push_back(name);
        }
        return names;
    }

    /** @brief Number of published sounds. */
    size_t size() const { return sounds_.size(); }

    /** @brief Forgets every published sound and the search dir. */
    void clear() {
        sounds_.clear();
        search_dir_.clear();
    }

private:
    std::string search_dir_;
    std::unordered_map<std::string, SoundDef> sounds_;
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_AUDIO_SOUND_LIBRARY_H
