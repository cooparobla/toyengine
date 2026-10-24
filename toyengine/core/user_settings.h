/**
 * @file user_settings.h
 * @brief Per-player settings that outlive a run -- audio volumes, interface scale, HUD layout:
 *        a small YAML document in the per-user data directory (runtime_paths.h's
 *        user_data_dir()/settings.yaml), never in the game's own, possibly read-only, install
 *        folder.
 *
 * Keys are flat dotted strings ("audio.music", "ui.hud_scale", "hud.Vitals.x"). Values a player
 * changed are remembered; anything never set falls back to the game's config.yaml defaults, which
 * is why every getter takes the fallback rather than inventing one. Writes happen only when
 * something changed (flush()), so a run that touches nothing never creates the file.
 *
 * Four value types are stored: float, int, bool and string. They are not interchangeable on read
 * -- get_bool() on a key written by set_float() returns nothing -- so a key keeps one type for
 * its lifetime.
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

    /** @brief The stored number, or nothing if `key` was never set (or holds a non-number). */
    std::optional<float> get_float(const std::string& key) const;
    float get_float(const std::string& key, float fallback) const { return get_float(key).value_or(fallback); }

    /** @brief The stored integer, or nothing if `key` was never set (or holds a non-number). */
    std::optional<int> get_int(const std::string& key) const;
    int get_int(const std::string& key, int fallback) const { return get_int(key).value_or(fallback); }

    /** @brief The stored flag, or nothing if `key` was never set (or holds a non-boolean). */
    std::optional<bool> get_bool(const std::string& key) const;
    bool get_bool(const std::string& key, bool fallback) const { return get_bool(key).value_or(fallback); }

    /** @brief The stored text, or nothing if `key` was never set (or holds a non-string). */
    std::optional<std::string> get_string(const std::string& key) const;
    std::string get_string(const std::string& key, const std::string& fallback) const {
        return get_string(key).value_or(fallback);
    }

    void set_float(const std::string& key, float value);
    void set_int(const std::string& key, int value);
    void set_bool(const std::string& key, bool value);
    void set_string(const std::string& key, const std::string& value);

    /** @brief Forgets `key`, so the next read falls back to the game's own default. */
    void erase(const std::string& key);

    /** @brief Whether `key` has a stored value of any type. */
    bool has(const std::string& key) const { return doc_.contains(key); }

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
