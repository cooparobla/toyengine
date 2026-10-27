#include <toyengine/core/user_settings.h>

#include <coopa/yaml/writer.h>

namespace toy {
namespace core {

UserSettings& UserSettings::instance() {
    static UserSettings s;
    return s;
}

void UserSettings::load(const std::filesystem::path& path) {
    path_ = path;
    dirty_ = false;
    doc_ = fkyaml::node::mapping();
    try {
        if (auto d = coopa::yaml::try_load_document(path); d && d->is_mapping()) doc_ = *d;
    } catch (const std::exception& e) {
        std::cerr << "[toyengine] Ignoring unreadable settings " << path.string() << ": " << e.what() << "\n";
    }
}

std::optional<float> UserSettings::get_float(const std::string& key) const {
    if (!doc_.contains(key)) return std::nullopt;
    const fkyaml::node& v = doc_[key];
    if (v.is_float_number()) return static_cast<float>(v.get_value<double>());
    if (v.is_integer()) return static_cast<float>(v.get_value<int64_t>());
    return std::nullopt;
}

std::optional<int> UserSettings::get_int(const std::string& key) const {
    if (!doc_.contains(key)) return std::nullopt;
    const fkyaml::node& v = doc_[key];
    if (v.is_integer()) return static_cast<int>(v.get_value<int64_t>());
    if (v.is_float_number()) return static_cast<int>(v.get_value<double>());
    return std::nullopt;
}

std::optional<bool> UserSettings::get_bool(const std::string& key) const {
    if (!doc_.contains(key)) return std::nullopt;
    const fkyaml::node& v = doc_[key];
    if (v.is_boolean()) return v.get_value<bool>();
    return std::nullopt;
}

std::optional<std::string> UserSettings::get_string(const std::string& key) const {
    if (!doc_.contains(key)) return std::nullopt;
    const fkyaml::node& v = doc_[key];
    if (v.is_string()) return v.get_value<std::string>();
    return std::nullopt;
}

void UserSettings::set_float(const std::string& key, float value) {
    if (auto cur = get_float(key); cur && *cur == value) return;
    doc_[key] = fkyaml::node(static_cast<double>(value));
    dirty_ = true;
    on_changed.emit(key);
}

void UserSettings::set_int(const std::string& key, int value) {
    if (auto cur = get_int(key); cur && *cur == value) return;
    doc_[key] = fkyaml::node(static_cast<int64_t>(value));
    dirty_ = true;
    on_changed.emit(key);
}

void UserSettings::set_bool(const std::string& key, bool value) {
    if (auto cur = get_bool(key); cur && *cur == value) return;
    doc_[key] = fkyaml::node(value);
    dirty_ = true;
    on_changed.emit(key);
}

void UserSettings::set_string(const std::string& key, const std::string& value) {
    if (auto cur = get_string(key); cur && *cur == value) return;
    doc_[key] = fkyaml::node(value);
    dirty_ = true;
    on_changed.emit(key);
}

void UserSettings::erase(const std::string& key) {
    if (!doc_.contains(key)) return;
    doc_.as_map().erase(key);
    dirty_ = true;
    on_changed.emit(key);
}

bool UserSettings::flush() {
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

const std::vector<std::string>& engine_setting_keys() {
    // Kept beside the code that applies them -- Engine::init_user_settings_() subscribes to
    // on_changed for the live ones and reads display.vsync at startup -- so the list cannot
    // drift from what actually happens. The audio spellings are volume_setting_key()'s:
    // "audio." plus the lower-cased bus name.
    static const std::vector<std::string> keys = {
        "audio.master", "audio.music", "audio.sfx", "audio.ui",
        "ui.hud_scale",
        "display.vsync",
    };
    return keys;
}

} // namespace core
} // namespace toy
