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

#include <caml/caml.h>

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
std::optional<SaveEncoding> parse_save_encoding(std::string s);

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
    static bool valid_slot_name(const std::string& slot);

    std::filesystem::path slot_path(const std::string& slot) const { return root() / (slot + ".save"); }
    std::filesystem::path meta_path(const std::string& slot) const { return root() / (slot + ".meta"); }

    bool has_slot(const std::string& slot) const;

    /** @brief Every slot, newest first. A slot whose sidecar is missing is described from its .save. */
    std::vector<SaveSlotInfo> list_slots() const;

    /** @brief One slot's description; nullopt if it does not exist or cannot be read. */
    std::optional<SaveSlotInfo> slot_info(const std::string& slot) const;

    /**
     * @brief Captures the current scene + global state into `slot` (replacing it).
     * @return False (see last_error()) for a bad slot name or a failed write.
     */
    bool save(const std::string& slot);

    /**
     * @brief Builds a document from `scene` (may be null): version, meta (scene, time,
     *        play_time), every ISaveable on a SaveId object, then on_save.
     */
    SaveGame capture(coopa::scene::Scene* scene);

    /** @brief Writes `game` to `slot` (the .save, then its .meta sidecar) atomically. */
    bool write(const std::string& slot, const SaveGame& game);

    /** @brief Reads `slot`, decoded and migrated to version(); nullopt (see last_error()) if it can't. */
    std::optional<SaveGame> read(const std::string& slot);

    /** @brief Runs the registered migrations from the document's version up to version(). */
    void migrate(SaveGame& game) const;

    /**
     * @brief Loads `slot`: when it was saved in another scene, that scene is loaded first
     *        (Host::load_scene, i.e. Engine::load_scene_async()) and the save applied once it is
     *        live; otherwise it is applied to the current scene now.
     * @return False if the slot could not be read or its scene could not start loading; true
     *         once applied or once the scene load started (on_loaded fires when done).
     */
    bool load(const std::string& slot);

    /**
     * @brief Applies a document to `scene` (may be null: global state only): on_load, then each
     *        ISaveable on a SaveId object that has a section in it.
     */
    void apply(const SaveGame& game, coopa::scene::Scene* scene);

    /** @brief Deletes `slot` (and its sidecar). False if there was nothing to delete. */
    bool delete_slot(const std::string& slot);

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

    static SaveSystem*& active_ref_();

    bool fail_(std::string why);

    void finish_load_(const SaveGame& game, coopa::scene::Scene* scene, const std::string& slot);

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

    static std::string iso_utc_(int64_t t);

    std::string passphrase_or_default_() const {
        return passphrase_.empty() ? core::detail::default_caml_passphrase() : passphrase_;
    }

    void write_document_(const std::filesystem::path& path, const fkyaml::node& root) const;

    /** @brief A plain or caml document (decoded here, so no codec registration is needed). */
    std::optional<fkyaml::node> read_document_(const std::filesystem::path& path) const;
};

}  // namespace save
}  // namespace toy

#endif  // TOYENGINE_SAVE_SAVE_SYSTEM_H
