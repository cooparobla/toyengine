#include <toyengine/save/save_system.h>

namespace toy {
namespace save {

std::optional<SaveEncoding> parse_save_encoding(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "yaml") return SaveEncoding::Yaml;
    if (s == "caml") return SaveEncoding::Caml;
    if (s == "auto" || s.empty()) return core::k_shipping ? SaveEncoding::Caml : SaveEncoding::Yaml;
    return std::nullopt;
}

bool SaveSystem::valid_slot_name(const std::string& slot) {
    if (slot.empty() || slot.size() > 128 || slot.front() == '.') return false;
    return std::all_of(slot.begin(), slot.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
    });
}

bool SaveSystem::has_slot(const std::string& slot) const {
    std::error_code ec;
    return valid_slot_name(slot) && std::filesystem::is_regular_file(slot_path(slot), ec);
}

std::vector<SaveSlotInfo> SaveSystem::list_slots() const {
    std::vector<SaveSlotInfo> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(root(), ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(root(), ec)) {
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".save") continue;
        if (auto info = slot_info(entry.path().stem().string())) out.push_back(std::move(*info));
    }
    std::sort(out.begin(), out.end(), [](const SaveSlotInfo& a, const SaveSlotInfo& b) {
        return a.timestamp != b.timestamp ? a.timestamp > b.timestamp : a.slot < b.slot;
    });
    return out;
}

std::optional<SaveSlotInfo> SaveSystem::slot_info(const std::string& slot) const {
    if (!has_slot(slot)) return std::nullopt;
    std::optional<fkyaml::node> meta;
    try {
        meta = read_document_(meta_path(slot));
        if (!meta) {   // no sidecar (or an older one lost): the save's own meta
            if (auto doc = read_document_(slot_path(slot))) {
                SaveGame g(*doc);
                meta = g.meta().node();
                (*meta)["version"] = fkyaml::node(static_cast<int64_t>(g.version()));
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[toyengine] save: unreadable slot '" << slot << "': " << e.what() << "\n";
        return std::nullopt;
    }
    if (!meta || !meta->is_mapping()) return std::nullopt;
    SaveNode m(&*meta, false);
    SaveSlotInfo info;
    info.slot = slot;
    info.path = slot_path(slot);
    info.scene = m.get<std::string>("scene", "");
    info.saved_at = m.get<std::string>("saved_at", "");
    info.timestamp = m.get<int64_t>("timestamp", 0);
    info.play_time = m.get<double>("play_time", 0.0);
    info.version = m.get<int>("version", 0);
    if (m.section("summary").valid()) info.summary = m.get_node("summary");
    return info;
}

bool SaveSystem::save(const std::string& slot) {
    error_.clear();
    if (!valid_slot_name(slot)) return fail_("invalid slot name '" + slot + "'");
    SaveGame game = capture(host_.scene ? host_.scene() : nullptr);
    SaveNode meta = game.meta();
    meta.set("slot", slot);
    return write(slot, game);
}

SaveGame SaveSystem::capture(coopa::scene::Scene* scene) {
    SaveGame game;
    game.set_version(version_);
    SaveNode meta = game.meta();
    meta.set("scene", host_.scene_path ? host_.scene_path() : std::string());
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    meta.set("timestamp", now);
    meta.set("saved_at", iso_utc_(now));
    meta.set("play_time", play_time_);
    if (scene) {
        std::set<std::string> seen;
        for_each_saved_object_(*scene, [&](const std::string& id, coopa::scene::SceneObject& obj) {
            if (!seen.insert(id).second) {
                std::cerr << "[toyengine] save: duplicate SaveId '" << id << "' (on '" << object_path(obj)
                          << "'); only the first is saved\n";
                return;
            }
            for (auto& comp : obj.components()) {
                auto* s = dynamic_cast<ISaveable*>(comp.get());
                if (!s) continue;
                SaveNode section = game.object(id).section(s->save_key());
                s->save(section);
            }
        });
    }
    on_save.emit(game);
    return game;
}

bool SaveSystem::write(const std::string& slot, const SaveGame& game) {
    error_.clear();
    if (!valid_slot_name(slot)) return fail_("invalid slot name '" + slot + "'");
    fkyaml::node meta = game.meta().node();
    meta["version"] = fkyaml::node(static_cast<int64_t>(game.version()));
    try {
        write_document_(slot_path(slot), game.root());
        write_document_(meta_path(slot), meta);
    } catch (const std::exception& e) {
        return fail_("could not write slot '" + slot + "': " + e.what());
    }
    return true;
}

std::optional<SaveGame> SaveSystem::read(const std::string& slot) {
    error_.clear();
    if (!has_slot(slot)) {
        fail_("no slot '" + slot + "' in " + root().string());
        return std::nullopt;
    }
    try {
        std::optional<fkyaml::node> doc = read_document_(slot_path(slot));
        if (!doc || !doc->is_mapping()) {
            fail_("slot '" + slot + "' is not a save document");
            return std::nullopt;
        }
        SaveGame game(std::move(*doc));
        migrate(game);
        return game;
    } catch (const std::exception& e) {
        fail_("could not read slot '" + slot + "': " + e.what());
        return std::nullopt;
    }
}

void SaveSystem::migrate(SaveGame& game) const {
    if (game.version() > version_) {
        std::cerr << "[toyengine] save: document version " << game.version() << " is newer than this build's "
                  << version_ << "; loading it as is\n";
        return;
    }
    while (game.version() < version_) {
        const int from = game.version();
        if (auto it = migrations_.find(from); it != migrations_.end() && it->second) it->second(game);
        game.set_version(from + 1);
    }
}

bool SaveSystem::load(const std::string& slot) {
    if (pending_) return fail_("a load is already in progress");
    std::optional<SaveGame> game = read(slot);
    if (!game) return false;
    const std::string scene = game->meta().get<std::string>("scene", "");
    const std::string current = host_.scene_path ? host_.scene_path() : std::string();
    if (!scene.empty() && scene != current && host_.load_scene) {
        auto doc = std::make_shared<SaveGame>(std::move(*game));
        pending_ = doc;
        const bool started = host_.load_scene(scene,
            [this, doc, slot](coopa::scene::Scene& s) {
                if (pending_ != doc) return;
                pending_.reset();
                finish_load_(*doc, &s, slot);
            },
            [this, doc, slot](const std::string& why) {
                if (pending_ != doc) return;
                pending_.reset();
                fail_("loading scene for slot '" + slot + "' failed: " + why);
            });
        if (!started) {
            pending_.reset();
            return fail_("could not start loading scene '" + scene + "' for slot '" + slot + "'");
        }
        return true;
    }
    finish_load_(*game, host_.scene ? host_.scene() : nullptr, slot);
    return true;
}

void SaveSystem::apply(const SaveGame& game, coopa::scene::Scene* scene) {
    on_load.emit(game);
    if (!scene) return;
    for_each_saved_object_(*scene, [&](const std::string& id, coopa::scene::SceneObject& obj) {
        const SaveNode saved = game.object(id);
        if (!saved.valid()) return;
        for (auto& comp : obj.components()) {
            auto* s = dynamic_cast<ISaveable*>(comp.get());
            if (!s) continue;
            const SaveNode section = saved.section(s->save_key());
            if (section.valid()) s->load(section);
        }
    });
}

bool SaveSystem::delete_slot(const std::string& slot) {
    error_.clear();
    if (!has_slot(slot)) return fail_("no slot '" + slot + "'");
    std::error_code ec;
    std::filesystem::remove(slot_path(slot), ec);
    if (ec) return fail_("could not delete slot '" + slot + "': " + ec.message());
    std::filesystem::remove(meta_path(slot), ec);
    return true;
}

SaveSystem*& SaveSystem::active_ref_() {
    static SaveSystem* s = nullptr;
    return s;
}

bool SaveSystem::fail_(std::string why) {
    error_ = std::move(why);
    std::cerr << "[toyengine] save: " << error_ << "\n";
    return false;
}

void SaveSystem::finish_load_(const SaveGame& game, coopa::scene::Scene* scene, const std::string& slot) {
    error_.clear();
    play_time_ = game.meta().get("play_time", play_time_);
    apply(game, scene);
    on_loaded.emit(slot);
}

std::string SaveSystem::iso_utc_(int64_t t) {
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

void SaveSystem::write_document_(const std::filesystem::path& path, const fkyaml::node& root) const {
    const std::string text = coopa::yaml::emit(root);
    if (encoding_ == SaveEncoding::Caml) {
        const std::vector<uint8_t> bytes = core::encode_caml_text(text, passphrase_or_default_());
        coopa::yaml::write_text_atomic(path, std::string(bytes.begin(), bytes.end()));
    } else {
        coopa::yaml::write_text_atomic(path, text);
    }
}

std::optional<fkyaml::node> SaveSystem::read_document_(const std::filesystem::path& path) const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string text;
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), caml::MAGIC, 4) == 0) {
        text = caml::decode(bytes, core::detail::caml_key(passphrase_or_default_()));
    } else {
        text.assign(bytes.begin(), bytes.end());
    }
    return fkyaml::node::deserialize(text);
}

} // namespace save
} // namespace toy
