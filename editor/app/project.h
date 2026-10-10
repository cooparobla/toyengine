/**
 * @file project.h
 * @brief A toyengine project on disk: a directory whose assets/ folder the editor builds.
 *
 * Layout the editor reads and writes -- the same one the game loads:
 * @code
 * <root>/<name>.toy                       (project file; optional -- see below)
 * <root>/assets/config.yaml
 *              scenes/<name>/scene.yaml   (+ scene-local meshes/, textures/)
 *              meshes/*.yaml              (+ <mesh>.lod.yaml sidecars)
 *              materials/*.yaml
 *              physics_materials/*.yaml
 *              textures/*.png
 * @endcode
 * Shaders and fonts come from the engine build, not the project.
 *
 * A project made by tools/toyhub also has a `<target>.toy` (YAML: `target`, and an `engine:`
 * block pinning the toyengine checkout in .libs/toyengine), src/ for its C++ and the build/run
 * scripts. A project's name is always its folder's name. A bare directory with an assets/
 * folder -- this repository itself -- is a project to the editor too.
 */

#ifndef TOYEDITOR_APP_PROJECT_H
#define TOYEDITOR_APP_PROJECT_H

#include "../core/yaml_util.h"
#include "../mesh/edit_mesh.h"
#include "../mesh/primitives.h"

#include <coopa/asset/asset_index.h>
#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

#include <root_directory.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace toy::editor {

namespace fs = std::filesystem;

/** @brief An assets-relative path without its .yaml / .caml extension (`objects/props/crate`). */
std::string strip_yaml_ext(const std::string& rel);

/**
 * @brief The type folder of an assets-relative path: `ui/themes` for a theme, else its first
 *        folder (`materials/metal/brick.yaml` -> `materials`). Everything between the type
 *        folder and the asset is TAGS (see asset_tags()).
 */
std::string asset_type_dir(const std::string& rel);

/** @brief True for a scene document in its own folder (`scenes/<tags>/<name>/scene*.yaml`). */
bool is_scene_folder_file(const std::string& rel);

/** @brief True for a scene-local file (`scenes/<scene>/meshes/x.yaml`): no tags, no short form. */
bool is_scene_local_file(const std::string& rel);

/**
 * @brief An asset's tags: the folders between its type folder and the asset (a scene's own
 *        folder is the asset, not a tag). `scenes/tests/water/water_test/scene.yaml` -> {tests,
 *        water}; `materials/brick.yaml` -> {}.
 */
std::vector<std::string> asset_tags(const std::string& rel);

/** @brief `rel` moved under the tag folders `tags` (same type folder, same asset name). */
std::string with_asset_tags(const std::string& rel, const std::vector<std::string>& tags);

/**
 * @brief The short reference to an asset: its type folder and name, without tag folders
 *        (`materials/metal/brick.yaml` -> `materials/brick.yaml`; a scene keeps its folder).
 *        The engine finds it by name wherever its tags put it (coopa::asset::AssetIndex), so
 *        references written this way survive re-tagging. Scene-local files are unchanged.
 */
std::string short_ref(const std::string& rel);

/**
 * @brief The `mesh_path` value naming a mesh file: its name -- the engine loads
 *        `meshes/<value>.yaml` from the scene's directory, then the assets roots, then finds it
 *        by name in any tag folder. `meshes/props/rock.yaml` -> `rock`;
 *        `scenes/lake/meshes/basin.yaml` -> `basin`.
 */
inline std::string mesh_ref(const std::string& rel) {
    return fs::path(strip_yaml_ext(fs::path(rel).generic_string())).filename().generic_string();
}

/**
 * @brief How references to a renamed / moved asset are rewritten. Paths that name their
 *        folder (`objects/x`, `materials/x`, `textures/x.png`, `ui/themes/x.yaml`) are
 *        unambiguous, so they are matched as whole string values under any key -- both the
 *        full path and the short one (short_ref()); a mesh is named by a bare mesh_ref() (or,
 *        written by older editors, its path under meshes/), so it is only matched under the
 *        keys that hold one. A move that keeps the name (re-tagging) changes no short form,
 *        so only full-path references need rewriting.
 */
struct RefRename {
    std::vector<std::pair<std::string, std::string>> paths;   ///< old -> new, any key
    std::vector<std::string> mesh_keys;                       ///< keys holding a mesh_ref()
    std::vector<std::pair<std::string, std::string>> meshes;  ///< old -> new mesh refs, under mesh_keys

    /** @brief The rewrite for renaming assets-relative `from` to `to` (empty if nothing refers to it by path). */
    static RefRename between(const std::string& from, const std::string& to);
    bool empty() const { return paths.empty() && meshes.empty(); }
    /** @brief Strings a file must contain to possibly refer to the asset (a cheap pre-check). */
    std::vector<std::string> needles() const;
    /** @brief Rewrites matching references in `n`; returns how many changed. */
    int apply(Node& n) const { return apply_(n, nullptr); }

private:
    int apply_(Node& n, const std::string* key) const;
};

class Project {
public:
    Project() = default;
    explicit Project(fs::path root) : root_(std::move(root)) {}

    const fs::path& root() const { return root_; }
    fs::path assets() const { return root_ / "assets"; }
    fs::path config_path() const { return assets() / "config.yaml"; }
    /** @brief A project has an assets/ folder, or a `.toy` file (assets/ not created yet). */
    bool valid() const;
    /** @brief True once assets/ exists (a fresh `.toy` project gets it from create()). */
    bool has_assets() const;
    /** @brief The project's name: always its folder's name. */
    std::string name() const;

    // --- the .toy project file ---

    /** @brief The project's `.toy` file (the first, alphabetically), or empty if it has none. */
    fs::path project_file() const { return find_project_file(root_); }
    static fs::path find_project_file(const fs::path& root);
    /** @brief A `.toy` document (an empty mapping if unreadable). */
    static Node load_toy(const fs::path& path);
    /** @brief The minimal `.toy` the editor writes for a project it creates (no engine pin --
     *         tools/toyhub adds that when it sets a project up for building). No name: a
     *         project's name is its folder's. */
    static Node default_toy();

    /** @brief `p` relative to assets/ (or, for a toyengine asset, to toyengine's assets/) with
     *         forward slashes, or `p` unchanged if outside both. */
    std::string relative(const fs::path& p) const;
    /**
     * @brief An assets-relative path's file: the project's, else -- like the engine's asset
     *        search roots -- toyengine's (when the project has no file there and toyengine does).
     *        Paths for NEW files are built from assets() directly, never from this.
     */
    fs::path absolute(const std::string& rel) const;

    // --- toyengine's own assets: the read-only layer under a game project's ---

    /** @brief toyengine's assets/ (the engine checkout this editor was built from). */
    static fs::path engine_assets() { return fs::path(ROOT_DIR) / "assets"; }
    /** @brief True when this project IS the toyengine checkout: its assets are all editable. */
    bool is_engine() const;
    /** @brief True for a file in toyengine's assets/ while editing another project: read-only. */
    bool is_engine_path(const fs::path& abs) const;
    /** @brief True when `rel` resolves to toyengine's copy (the project has none). */
    bool is_engine_asset(const std::string& rel) const { return is_engine_path(absolute(rel)); }

    /**
     * @brief toyengine's assets under <dir> with `ext`, like list(), minus those the project has
     *        its own copy of (that copy is what resolves). Empty when this project is toyengine.
     */
    const std::vector<std::string>& list_engine(const std::string& dir, const std::string& ext);
    /** @brief toyengine's scenes (see scenes()) the project doesn't have. */
    std::vector<std::string> engine_scenes() { return scene_files_(list_engine("scenes", ".yaml")); }

    /**
     * @brief Every file under assets/<dir> with extension `ext` (a .yaml request also
     *        matches .caml), as assets-relative paths, sorted. Cached; refresh() rescans.
     */
    const std::vector<std::string>& list(const std::string& dir, const std::string& ext);
    void refresh();

    /**
     * @brief Renames / moves assets-relative `from` to `to` and rewrites every reference to it
     *        in the project's YAML (scenes, object / UI assets, materials, config, LOD sidecars).
     *        A mesh's `.lod.yaml` sidecar moves with it; a scene in its own folder
     *        (is_scene_folder_file()) moves as the whole folder -- its scene-local files and
     *        sibling scene documents along. Returns the files rewritten (assets-relative);
     *        throws on a failed rename.
     */
    std::vector<std::string> rename_asset(const std::string& from, const std::string& to);

    /** @brief All scene files (assets/scenes/<name>/scene.yaml and any other .yaml in scenes/). */
    std::vector<std::string> scenes() { return scene_files_(list("scenes", ".yaml")); }

    // --- recent projects (~/.toyengine_editor.yaml) ---

    static fs::path prefs_path();
    static Node load_prefs();
    static void save_prefs(const Node& prefs) {
        try { coopa::yaml::save_document(prefs_path(), prefs); } catch (...) {}
    }
    static std::vector<std::string> recent_projects();
    static void remember(const fs::path& root);

    /**
     * @brief Creates a new project skeleton at `root`: a `.toy` (unless it has one), folders,
     *        a config.yaml, starter meshes and a default scene with a camera, a sun and a
     *        ground plane.
     * @return The project. Existing files are never overwritten.
     */
    static Project create(const fs::path& root);

    /**
     * @brief The engine's assets/config.yaml with `window.title` set to `title`,
     *        `scene.default_scene` pointing at the new project's main scene and screenshots on
     *        exit off; every other setting as the engine ships it. Read and written as a YAML
     *        document -- the same load / save the editor's config panel uses -- so any layout of
     *        the engine's file (hand-written, or as the editor saves it) works.
     */
    static Node default_config(const std::string& title);

    /** @brief A starter scene document: camera, sun, ground plane, a cube -- named in snake_case,
     *         like everything in assets/. */
    static Node default_scene_node(const std::string& name);

private:
    /** @brief Files under <root>/<dir> with `ext` (.yaml also matches .caml), relative to `root`, sorted. */
    static std::vector<std::string> scan_(const fs::path& root, const std::string& dir, const std::string& ext);
    /** @brief The scene documents among scenes/ files (not their scene-local meshes). */
    static std::vector<std::string> scene_files_(const std::vector<std::string>& files);
    static bool exists_(const fs::path& p);

    fs::path root_;
    std::map<std::string, std::vector<std::string>> cache_;
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_PROJECT_H
