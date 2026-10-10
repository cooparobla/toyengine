/**
 * @file user_settings.h
 * @brief Per-player settings that outlive a run (audio volumes today): a small YAML document in
 *        the per-user data directory (runtime_paths.h's user_data_dir()/settings.yaml), never in
 *        the game's own, possibly read-only, install folder.
 *
 * Keys are flat dotted strings ("audio.music"). Values a player changed are remembered; anything
 * never set falls back to the game's config.yaml defaults. Writes happen only when something
 * changed (flush()), so a run that touches nothing never creates the file.
 */

#ifndef TOYENGINE_CORE_USER_SETTINGS_H
#define TOYENGINE_CORE_USER_SETTINGS_H

#include <toyengine/core/runtime_paths.h>

#include <coopa/yaml/document.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

namespace toy::core {

class UserSettings {
public:
    /** @brief The process-wide settings (loaded by Engine at startup). */
    static UserSettings& instance();

    /** @brief The default location: <user_data_dir>/settings.yaml. */
    static std::filesystem::path default_path() { return user_data_dir() / "settings.yaml"; }

    /** @brief (Re)loads from `path`; a missing or unreadable file means "nothing set". */
    void load(const std::filesystem::path& path = default_path());

    std::optional<float> get_float(const std::string& key) const;
    float get_float(const std::string& key, float fallback) const { return get_float(key).value_or(fallback); }

    void set_float(const std::string& key, float value);

    bool dirty() const { return dirty_; }
    const std::filesystem::path& path() const { return path_; }

    /** @brief Writes the file if anything changed since load / the last flush. */
    bool flush();

private:
    fkyaml::node doc_ = fkyaml::node::mapping();
    std::filesystem::path path_;
    bool dirty_ = false;
};

} // namespace toy::core

#endif // TOYENGINE_CORE_USER_SETTINGS_H
