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

void UserSettings::set_float(const std::string& key, float value) {
    if (auto cur = get_float(key); cur && *cur == value) return;
    doc_[key] = fkyaml::node(static_cast<double>(value));
    dirty_ = true;
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

} // namespace core
} // namespace toy
