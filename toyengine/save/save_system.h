/**
 * @file save_system.h
 * @brief SaveSystem: where saves live and how they are written, read and applied. What goes
 *        in them is up to the game (see save_game.h and saveable.h).
 *
 * Reached through `engine.saves()` (or SaveSystem::active() from a component). Slots are files
 * in one directory, by default `<user_data_dir>/saves/` (runtime_paths.h):
 *
 *   <slot>.save   the whole SaveGame document
 *   <slot>.meta   a small sidecar for slot menus: version, scene, saved_at, play_time, summary
 *
 * Both are written atomically (a temp file, then a rename), so a crash mid-save leaves the
 * previous save intact. They are plain YAML in development and caml-encoded in a TOY_SHIPPING
 * build (config.yaml `save.encode: auto | yaml | caml`); reading accepts either.
 *
 * The game fills the document through three opt-in hooks:
 *  - `on_save(SaveGame&)` / `on_load(const SaveGame&)` signals for global state (its `global`
 *    section, and meta `summary` for slot menus);
 *  - ISaveable components on objects with a SaveId, each saving its own section;
 *  - `register_migration(from_version, fn)` to upgrade documents written by older builds
 *    (set_version() is the current format), run before anything reads them.
 *
 * @code
 * auto& saves = engine.saves();
 * saves.set_version(2);
 * saves.register_migration(1, [](SaveGame& g) { g.global().set("gold", g.global().get("coins", 0)); });
 * conn_ = saves.on_save.connect_scoped([&](SaveGame& g) {
 *     g.global().set("gold", gold);
 *     g.summary().set("level", level_name);   // shown by a slot menu via list_slots()
 * });
 * conn2_ = saves.on_load.connect_scoped([&](const SaveGame& g) { gold = g.global().get("gold", 0); });
 * saves.save("slot_1");
 * saves.load("slot_1");     // loads the saved scene first if it is not the current one
 * @endcode
 *
 * Load order: read + decode, migrations, then -- in the saved scene, after it started (a scene
 * change goes through Engine::load_scene_async()) and before its next update -- `on_load`, then
 * every ISaveable, then `on_loaded`. `on_load` running first lets the game respawn runtime
 * objects (with their SaveIds) that the ISaveable pass then restores.
 */

#ifndef TOYENGINE_SAVE_SAVE_SYSTEM_H
#define TOYENGINE_SAVE_SAVE_SYSTEM_H

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <coopa/event/signal.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

#include <toyengine/core/caml_codec.h>
#include <toyengine/core/runtime_paths.h>
#include <toyengine/save/save_game.h>
#include <toyengine/save/saveable.h>

namespace toy {
namespace save {

/** @brief How slot files are written. Reading always accepts both. */
enum class SaveEncoding { Yaml, Caml };

/** @brief "yaml" / "caml" / "auto" (caml in a TOY_SHIPPING build, else yaml); nullopt otherwise. */
inline std::optional<SaveEncoding> parse_save_encoding(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "yaml") return SaveEncoding::Yaml;
    if (s == "caml") return SaveEncoding::Caml;
    if (s == "auto" || s.empty()) return core::k_shipping ? SaveEncoding::Caml : SaveEncoding::Yaml;
    return std::nullopt;
}

/** @brief One slot as list_slots() reports it (read from its .meta sidecar). */
struct SaveSlotInfo {
    std::string           slot;
    std::filesystem::path path;           ///< The .save file.
    std::string           scene;          ///< The scene it was saved in ("" if none).
    std::string           saved_at;       ///< UTC, ISO 8601 ("2026-10-08T14:03:12Z").
    int64_t               timestamp = 0;  ///< Seconds since the Unix epoch.
    double                play_time = 0;  ///< Seconds of play when saved.
    int                   version = 0;    ///< The save format version it was written with.
    fkyaml::node          summary = fkyaml::node::mapping();   ///< The game's meta.summary.

    /** @brief Typed read access to `summary`. */
    SaveNode summary_node() const { return SaveNode(const_cast<fkyaml::node*>(&summary), false); }
};

/**
 * @class SaveSystem
 * @brief Slot files (list / save / load / delete) plus the hooks that fill and apply them.
 */
class SaveSystem {
public:
    /** @brief What the system needs from its owner (the Engine wires these). */
    struct Host {
        /// The scene saves are taken from and applied to.
        std::function<coopa::scene::Scene*()> scene;
        /// The current scene's path as stored in saves ("" if it has none).
        std::function<std::string()> scene_path;
        /// Starts loading a saved scene; calls `ready` once it is live (before its first
        /// update) or `failed`. False if it could not start.
        std::function<bool(const std::string& scene, std::function<void(coopa::scene::Scene&)> ready,
                           std::function<void(const std::string&)> failed)> load_scene;
    };

    /** @brief The engine's save system (set by Engine), for components; null without one. */
    static SaveSystem* active() { return active_ref_(); }
    static void set_active(SaveSystem* s) { active_ref_() = s; }

    // --- Hooks --------------------------------------------------------------------------
    coopa::event::Signal<SaveGame&>          on_save;    ///< After ISaveables wrote; before writing.
    coopa::event::Signal<const SaveGame&>    on_load;    ///< Before ISaveables are restored.
    coopa::event::Signal<const std::string&> on_loaded;  ///< A load finished (the slot name).

    /** @brief Runs `fn` on documents of version `from_version`, upgrading them to from_version + 1. */
    void register_migration(int from_version, std::function<void(SaveGame&)> fn) {
        migrations_[from_version] = std::move(fn);
    }

    // --- Settings -----------------------------------------------------------------------
    void set_host(Host host) { host_ = std::move(host); }

    /** @brief The slot directory; default <user_data_dir>/saves (created on first save). */
    std::filesystem::path root() const { return root_.empty() ? core::user_data_dir() / "saves" : root_; }
    void set_root(std::filesystem::path dir) { root_ = std::move(dir); }

    SaveEncoding encoding() const { return encoding_; }
    void set_encoding(SaveEncoding e) { encoding_ = e; }
    /** @brief The caml passphrase (empty: the game's asset passphrase, see caml_codec.h). */
    void set_passphrase(std::string p) { passphrase_ = std::move(p); }

    /** @brief The current save format version: written to new saves, migrated up to on load. */
    int version() const { return version_; }
    void set_version(int v) { version_ = v; }

    /** @brief Seconds of play, carried in saves (the Engine ticks it while a scene simulates). */
    double play_time() const { return play_time_; }
    void set_play_time(double t) { play_time_ = t; }
    void tick(float dt) { if (dt > 0.0f) play_time_ += dt; }

    /** @brief True while load() waits for its scene to finish loading. */
    bool loading() const { return pending_ != nullptr; }
    /** @brief Why the last save / load / delete failed ("" after a success). */
    const std::string& last_error() const { return error_; }

    // --- Slots --------------------------------------------------------------------------

    /** @brief Letters, digits, '_', '-' and '.' (not leading); no path separators. */
    static bool valid_slot_name(const std::string& slot) {
        if (slot.empty() || slot.size() > 128 || slot.front() == '.') return false;
        return std::all_of(slot.begin(), slot.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
        });
    }

    std::filesystem::path slot_path(const std::string& slot) const { return root() / (slot + ".save"); }
    std::filesystem::path meta_path(const std::string& slot) const { return root() / (slot + ".meta"); }

    bool has_slot(const std::string& slot) const {
        std::error_code ec;
        return valid_slot_name(slot) && std::filesystem::is_regular_file(slot_path(slot), ec);
    }

    /** @brief Every slot, newest first. A slot whose sidecar is missing is described from its .save. */
    std::vector<SaveSlotInfo> list_slots() const {
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

    /** @brief One slot's description; nullopt if it does not exist or cannot be read. */
    std::optional<SaveSlotInfo> slot_info(const std::string& slot) const {
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

    /**
     * @brief Captures the current scene + global state into `slot` (replacing it).
     * @return False (see last_error()) for a bad slot name or a failed write.
     */
    bool save(const std::string& slot) {
        error_.clear();
        if (!valid_slot_name(slot)) return fail_("invalid slot name '" + slot + "'");
        SaveGame game = capture(host_.scene ? host_.scene() : nullptr);
        SaveNode meta = game.meta();
        meta.set("slot", slot);
        return write(slot, game);
    }

    /**
     * @brief Builds a document from `scene` (may be null): version, meta (scene, time,
     *        play_time), every ISaveable on a SaveId object, then on_save.
     */
    SaveGame capture(coopa::scene::Scene* scene) {
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

    /** @brief Writes `game` to `slot` (the .save, then its .meta sidecar) atomically. */
    bool write(const std::string& slot, const SaveGame& game) {
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

    /** @brief Reads `slot`, decoded and migrated to version(); nullopt (see last_error()) if it can't. */
    std::optional<SaveGame> read(const std::string& slot) {
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

    /** @brief Runs the registered migrations from the document's version up to version(). */
    void migrate(SaveGame& game) const {
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

    /**
     * @brief Loads `slot`: when it was saved in another scene, that scene is loaded first
     *        (Host::load_scene, i.e. Engine::load_scene_async()) and the save applied once it is
     *        live; otherwise it is applied to the current scene now.
     * @return False if the slot could not be read or its scene could not start loading; true
     *         once applied or once the scene load started (on_loaded fires when done).
     */
    bool load(const std::string& slot) {
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

    /**
     * @brief Applies a document to `scene` (may be null: global state only): on_load, then each
     *        ISaveable on a SaveId object that has a section in it.
     */
    void apply(const SaveGame& game, coopa::scene::Scene* scene) {
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

    /** @brief Deletes `slot` (and its sidecar). False if there was nothing to delete. */
    bool delete_slot(const std::string& slot) {
        error_.clear();
        if (!has_slot(slot)) return fail_("no slot '" + slot + "'");
        std::error_code ec;
        std::filesystem::remove(slot_path(slot), ec);
        if (ec) return fail_("could not delete slot '" + slot + "': " + ec.message());
        std::filesystem::remove(meta_path(slot), ec);
        return true;
    }

    /** @brief Forgets a pending cross-scene load (its scene load was cancelled or superseded). */
    void cancel_pending_load() { pending_.reset(); }

private:
    Host host_;
    std::filesystem::path root_;
    SaveEncoding encoding_ = core::k_shipping ? SaveEncoding::Caml : SaveEncoding::Yaml;
    std::string passphrase_;
    int version_ = 1;
    double play_time_ = 0.0;
    std::map<int, std::function<void(SaveGame&)>> migrations_;
    std::shared_ptr<SaveGame> pending_;
    std::string error_;

    static SaveSystem*& active_ref_() {
        static SaveSystem* s = nullptr;
        return s;
    }

    bool fail_(std::string why) {
        error_ = std::move(why);
        std::cerr << "[toyengine] save: " << error_ << "\n";
        return false;
    }

    void finish_load_(const SaveGame& game, coopa::scene::Scene* scene, const std::string& slot) {
        error_.clear();
        play_time_ = game.meta().get("play_time", play_time_);
        apply(game, scene);
        on_loaded.emit(slot);
    }

    /** @brief Every object with a SaveId (active or not), with its key. */
    template<typename Fn>
    static void for_each_saved_object_(coopa::scene::Scene& scene, Fn&& fn) {
        for (auto& root : scene.root_objects()) {
            root->for_each_recursive([&](coopa::scene::SceneObject& obj) {
                if (auto* sid = obj.get_component<SaveId>()) {
                    const std::string key = sid->key();
                    if (!key.empty()) fn(key, obj);
                }
            });
        }
    }

    static std::string iso_utc_(int64_t t) {
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

    std::string passphrase_or_default_() const {
        return passphrase_.empty() ? core::detail::default_caml_passphrase() : passphrase_;
    }

    void write_document_(const std::filesystem::path& path, const fkyaml::node& root) const {
        const std::string text = coopa::yaml::emit(root);
        if (encoding_ == SaveEncoding::Caml) {
            const std::vector<uint8_t> bytes = core::encode_caml_text(text, passphrase_or_default_());
            coopa::yaml::write_text_atomic(path, std::string(bytes.begin(), bytes.end()));
        } else {
            coopa::yaml::write_text_atomic(path, text);
        }
    }

    /** @brief A plain or caml document (decoded here, so no codec registration is needed). */
    std::optional<fkyaml::node> read_document_(const std::filesystem::path& path) const {
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
};

}  // namespace save
}  // namespace toy

#endif  // TOYENGINE_SAVE_SAVE_SYSTEM_H
