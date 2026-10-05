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
#include <coopa/yaml/writer.h>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>

namespace toy::core {

class UserSettings {
public:
    /** @brief The process-wide settings (loaded by Engine at startup). */
    static UserSettings& instance() {
        static UserSettings s;
        return s;
    }

    /** @brief The default location: <user_data_dir>/settings.yaml. */
    static std::filesystem::path default_path() { return user_data_dir() / "settings.yaml"; }

    /** @brief (Re)loads from `path`; a missing or unreadable file means "nothing set". */
    void load(const std::filesystem::path& path = default_path()) {
        path_ = path;
        dirty_ = false;
        doc_ = fkyaml::node::mapping();
        try {
            if (auto d = coopa::yaml::try_load_document(path); d && d->is_mapping()) doc_ = *d;
        } catch (const std::exception& e) {
            std::cerr << "[toyengine] Ignoring unreadable settings " << path.string() << ": " << e.what() << "\n";
        }
    }

    std::optional<float> get_float(const std::string& key) const {
        if (!doc_.contains(key)) return std::nullopt;
        const fkyaml::node& v = doc_[key];
        if (v.is_float_number()) return static_cast<float>(v.get_value<double>());
        if (v.is_integer()) return static_cast<float>(v.get_value<int64_t>());
        return std::nullopt;
    }
    float get_float(const std::string& key, float fallback) const { return get_float(key).value_or(fallback); }

    void set_float(const std::string& key, float value) {
        if (auto cur = get_float(key); cur && *cur == value) return;
        doc_[key] = fkyaml::node(static_cast<double>(value));
        dirty_ = true;
    }

    bool dirty() const { return dirty_; }
    const std::filesystem::path& path() const { return path_; }

    /** @brief Writes the file if anything changed since load / the last flush. */
    bool flush() {
        if (!dirty_ || path_.empty()) return true;
        std::error_code ec;
        std::filesystem::create_directories(path_.parent_path(), ec);
        try {
            coopa::yaml::save_document(path_, doc_);
            dirty_ = false;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[toyengine] Could not save settings to " << path_.string() << ": " << e.what() << "\n";
            return false;
        }
    }

private:
    fkyaml::node doc_ = fkyaml::node::mapping();
    std::filesystem::path path_;
    bool dirty_ = false;
};

} // namespace toy::core

#endif // TOYENGINE_CORE_USER_SETTINGS_H
