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
inline std::string strip_yaml_ext(const std::string& rel) {
    const std::string e = fs::path(rel).extension().string();
    return (e == ".yaml" || e == ".caml") ? rel.substr(0, rel.size() - e.size()) : rel;
}

/**
 * @brief The `mesh_path` value naming a mesh file: its path under the `meshes/` folder it lives
 *        in, without the extension -- the engine loads `meshes/<value>.yaml` from the scene's
 *        directory, then the assets root. `meshes/props/rock.yaml` -> `props/rock`;
 *        `scenes/lake/meshes/basin.yaml` -> `basin`.
 */
inline std::string mesh_ref(const std::string& rel) {
    const std::string g = strip_yaml_ext(fs::path(rel).generic_string());
    if (g.rfind("meshes/", 0) == 0) return g.substr(7);
    const size_t at = g.find("/meshes/");
    if (at != std::string::npos) return g.substr(at + 8);
    return fs::path(g).filename().string();
}

/**
 * @brief How references to a renamed asset are rewritten. Paths that name their folder
 *        (`objects/x`, `materials/x`, `textures/x.png`, `ui/themes/x.yaml`) are unambiguous, so
 *        they are matched as whole string values under any key; a mesh is named by a bare
 *        `mesh_ref()`, so it is only matched under the keys that hold one.
 */
struct RefRename {
    std::vector<std::pair<std::string, std::string>> paths;   ///< old -> new, any key
    std::vector<std::string> mesh_keys;                       ///< keys holding a mesh_ref()
    std::string mesh_old, mesh_new;

    /** @brief The rewrite for renaming assets-relative `from` to `to` (empty if nothing refers to it by path). */
    static RefRename between(const std::string& from, const std::string& to) {
        RefRename r;
        const std::string f = fs::path(from).generic_string(), t = fs::path(to).generic_string();
        if (f.find("meshes/") != std::string::npos) {
            r.mesh_keys = {"mesh_path", "side_mesh", "mesh"};
            r.mesh_old = mesh_ref(f);
            r.mesh_new = mesh_ref(t);
            return r;
        }
        r.paths.push_back({f, t});
        r.paths.push_back({"assets/" + f, "assets/" + t});   // config.yaml's root-relative refs
        if (strip_yaml_ext(f) != f) r.paths.push_back({strip_yaml_ext(f), strip_yaml_ext(t)});
        return r;
    }
    /** @brief Strings a file must contain to possibly refer to the asset (a cheap pre-check). */
    std::vector<std::string> needles() const {
        std::vector<std::string> out;
        for (const auto& p : paths) out.push_back(p.first);
        if (!mesh_old.empty()) out.push_back(mesh_old);
        return out;
    }
    /** @brief Rewrites matching references in `n`; returns how many changed. */
    int apply(Node& n) const { return apply_(n, nullptr); }

private:
    int apply_(Node& n, const std::string* key) const {
        int changed = 0;
        if (n.is_string()) {
            const std::string v = n.get_value<std::string>();
            for (const auto& p : paths) {
                if (v == p.first) { n = Node(p.second); return 1; }
            }
            if (key && !mesh_old.empty() && v == mesh_old &&
                std::find(mesh_keys.begin(), mesh_keys.end(), *key) != mesh_keys.end()) {
                n = Node(mesh_new);
                return 1;
            }
        } else if (n.is_mapping()) {
            for (auto& kv : n.as_map()) {
                const std::string k = kv.first.is_string() ? kv.first.get_value<std::string>() : std::string();
                changed += apply_(kv.second, &k);
            }
        } else if (n.is_sequence()) {
            for (auto& item : n.as_seq()) changed += apply_(item, key);
        }
        return changed;
    }
};

class Project {
public:
    Project() = default;
    explicit Project(fs::path root) : root_(std::move(root)) {}

    const fs::path& root() const { return root_; }
    fs::path assets() const { return root_ / "assets"; }
    fs::path config_path() const { return assets() / "config.yaml"; }
    /** @brief A project has an assets/ folder, or a `.toy` file (assets/ not created yet). */
    bool valid() const { std::error_code ec; return fs::is_directory(assets(), ec) || !project_file().empty(); }
    /** @brief True once assets/ exists (a fresh `.toy` project gets it from create()). */
    bool has_assets() const { std::error_code ec; return fs::is_directory(assets(), ec); }
    /** @brief The project's name: always its folder's name. */
    std::string name() const {
        const fs::path r = root_.has_filename() ? root_ : root_.parent_path();   // "/x/y/" -> "y"
        return r.filename().string();
    }

    // --- the .toy project file ---

    /** @brief The project's `.toy` file (the first, alphabetically), or empty if it has none. */
    fs::path project_file() const { return find_project_file(root_); }
    static fs::path find_project_file(const fs::path& root) {
        std::error_code ec;
        std::vector<fs::path> found;
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().extension() == ".toy") found.push_back(it->path());
        }
        std::sort(found.begin(), found.end());
        return found.empty() ? fs::path() : found.front();
    }
    /** @brief A `.toy` document (an empty mapping if unreadable). */
    static Node load_toy(const fs::path& path) {
        try { if (auto n = coopa::yaml::try_load_document(path)) return *n; } catch (...) {}
        return Node::mapping();
    }
    /** @brief The minimal `.toy` the editor writes for a project it creates (no engine pin --
     *         tools/toyhub adds that when it sets a project up for building). No name: a
     *         project's name is its folder's. */
    static Node default_toy() {
        Node toy = Node::mapping();
        toy["format"] = Node(std::string("toyproject"));
        toy["version"] = Node(int64_t(1));
        return toy;
    }

    /** @brief `p` relative to assets/ with forward slashes, or `p` unchanged if outside. */
    std::string relative(const fs::path& p) const {
        std::error_code ec;
        const fs::path rel = fs::relative(p, assets(), ec);
        const std::string s = rel.generic_string();
        if (ec || s.empty() || s.rfind("..", 0) == 0) return p.generic_string();
        return s;
    }
    fs::path absolute(const std::string& rel) const {
        const fs::path p(rel);
        return p.is_absolute() ? p : assets() / p;
    }

    /**
     * @brief Every file under assets/<dir> with extension `ext` (a .yaml request also
     *        matches .caml), as assets-relative paths, sorted. Cached; refresh() rescans.
     */
    const std::vector<std::string>& list(const std::string& dir, const std::string& ext) {
        const std::string key = dir + "|" + ext;
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;
        std::vector<std::string> out;
        std::error_code ec;
        const fs::path base = dir.empty() ? assets() : assets() / dir;
        if (fs::is_directory(base, ec)) {
            for (auto e = fs::recursive_directory_iterator(base, ec); e != fs::recursive_directory_iterator(); e.increment(ec)) {
                if (ec) break;
                if (!e->is_regular_file()) continue;
                const std::string fe = e->path().extension().string();
                const std::string fname = e->path().filename().string();
                if (fname.find(".lod.") != std::string::npos) continue;   // sidecars aren't meshes
                if (!ext.empty() && fe != ext && !(ext == ".yaml" && fe == ".caml")) continue;
                out.push_back(relative(e->path()));
            }
        }
        std::sort(out.begin(), out.end());
        return cache_[key] = out;
    }
    void refresh() { cache_.clear(); }

    /**
     * @brief Renames assets-relative `from` to `to` and rewrites every reference to it in the
     *        project's YAML (scenes, object / UI assets, materials, config, LOD sidecars). A
     *        mesh's `.lod.yaml` sidecar moves with it. Returns the files rewritten
     *        (assets-relative); throws on a failed rename.
     */
    std::vector<std::string> rename_asset(const std::string& from, const std::string& to) {
        const fs::path src = absolute(from), dst = absolute(to);
        fs::create_directories(dst.parent_path());
        fs::rename(src, dst);
        std::error_code ec;
        if (from.find("meshes/") != std::string::npos) {
            const fs::path lod_src = src.parent_path() / (src.stem().string() + ".lod" + src.extension().string());
            if (fs::exists(lod_src, ec)) {
                fs::rename(lod_src, dst.parent_path() / (dst.stem().string() + ".lod" + dst.extension().string()), ec);
            }
        }
        const RefRename r = RefRename::between(from, to);
        const auto needles = r.needles();
        const bool mesh = !r.mesh_old.empty();
        // A scene-local mesh (scenes/<s>/meshes/x.yaml) is only visible to its own scene's files;
        // a global one is shadowed in any directory that has its own meshes/<ref>.yaml.
        const std::string g = fs::path(from).generic_string();
        const bool local_mesh = mesh && g.rfind("meshes/", 0) != 0;
        const fs::path scan_root = local_mesh ? assets() / g.substr(0, g.find("/meshes/")) : assets();
        std::vector<std::string> rewritten;
        for (auto e = fs::recursive_directory_iterator(scan_root, ec); e != fs::recursive_directory_iterator(); e.increment(ec)) {
            if (ec) break;
            if (!e->is_regular_file() || e->path().extension() != ".yaml") continue;   // .caml is a packaged copy
            if (mesh && !local_mesh && e->path().parent_path() != assets() &&
                fs::exists(e->path().parent_path() / "meshes" / (r.mesh_old + ".yaml"), ec)) continue;
            // Mesh files are large and only refer to other meshes in `lods:`.
            const bool in_meshes = e->path().parent_path().filename() == "meshes";
            if (in_meshes && !mesh) continue;
            std::string text;
            {
                std::ifstream in(e->path(), std::ios::binary);
                text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            if (in_meshes && text.find("lods:") == std::string::npos) continue;
            if (std::none_of(needles.begin(), needles.end(), [&](const std::string& n) { return text.find(n) != std::string::npos; })) continue;
            try {
                Node doc = coopa::yaml::load_document(e->path());
                if (r.apply(doc) == 0) continue;
                coopa::yaml::save_document(e->path(), doc);
                rewritten.push_back(relative(e->path()));
            } catch (...) {
                // An unparsable file is left alone; the engine can't load it either.
            }
        }
        refresh();
        return rewritten;
    }

    /** @brief All scene files (assets/scenes/<name>/scene.yaml and any other .yaml in scenes/). */
    std::vector<std::string> scenes() {
        std::vector<std::string> out;
        for (const auto& p : list("scenes", ".yaml")) {
            const std::string f = fs::path(p).filename().string();
            if (fs::path(p).parent_path().filename() == "meshes") continue;
            if (f.rfind("scene", 0) == 0 || fs::path(p).parent_path() == "scenes") out.push_back(p);
        }
        return out;
    }

    // --- recent projects (~/.toyengine_editor.yaml) ---

    static fs::path prefs_path() {
        const char* home = std::getenv("HOME");
        return fs::path(home ? home : ".") / ".toyengine_editor.yaml";
    }
    static Node load_prefs() {
        try { if (auto n = coopa::yaml::try_load_document(prefs_path())) return *n; } catch (...) {}
        return Node::mapping();
    }
    static void save_prefs(const Node& prefs) {
        try { coopa::yaml::save_document(prefs_path(), prefs); } catch (...) {}
    }
    static std::vector<std::string> recent_projects() {
        std::vector<std::string> out;
        const Node prefs = load_prefs();
        if (prefs.contains("recent_projects")) {
            for (const auto& r : prefs.at("recent_projects").as_seq()) if (r.is_string()) out.push_back(r.get_value<std::string>());
        }
        return out;
    }
    static void remember(const fs::path& root) {
        Node prefs = load_prefs();
        std::vector<std::string> recent = recent_projects();
        const std::string s = fs::absolute(root).lexically_normal().string();
        recent.erase(std::remove(recent.begin(), recent.end(), s), recent.end());
        recent.insert(recent.begin(), s);
        if (recent.size() > 8) recent.resize(8);
        Node seq = Node::sequence();
        for (const auto& r : recent) seq.as_seq().push_back(Node(r));
        prefs["recent_projects"] = seq;
        save_prefs(prefs);
    }

    /**
     * @brief Creates a new project skeleton at `root`: a `.toy` (unless it has one), folders,
     *        a config.yaml, starter meshes and a default scene with a camera, a sun and a
     *        ground plane.
     * @return The project. Existing files are never overwritten.
     */
    static Project create(const fs::path& root) {
        Project p(root);
        std::error_code ec;
        fs::create_directories(root, ec);
        if (p.project_file().empty()) {
            const std::string n = p.name().empty() ? std::string("project") : p.name();
            coopa::yaml::save_document(root / (n + ".toy"), default_toy());
        }
        for (const char* d : {"scenes/main", "objects", "meshes", "materials", "physics_materials", "textures", "ui"}) {
            fs::create_directories(p.assets() / d, ec);
        }
        auto write_if_missing = [&](const fs::path& path, const Node& node) {
            if (!fs::exists(path, ec)) coopa::yaml::save_document(path, node);
        };
        // config.yaml: the engine's own (ROOT_DIR/assets/config.yaml), copied as text so its
        // comments come along -- only the window title and the default scene are this project's.
        // Paths it names that the project lacks (the palette) resolve to the engine's assets/.
        if (!fs::exists(p.config_path(), ec)) {
            std::ofstream(p.config_path()) << default_config_text(p.name().empty() ? std::string("toyengine") : p.name());
        }

        write_if_missing(p.assets() / "meshes" / "cube.yaml", mesh_to_node(make_cube()));
        write_if_missing(p.assets() / "meshes" / "plane.yaml", mesh_to_node(make_plane(1.0f)));
        write_if_missing(p.assets() / "meshes" / "sphere.yaml", mesh_to_node(make_uv_sphere()));

        Node mat = Node::mapping();
        mat["albedo"] = make_color(glm::vec3(0.8f));
        mat["metallic"] = make_float(0.0);
        mat["roughness"] = make_float(0.6);
        write_if_missing(p.assets() / "materials" / "default.yaml", mat);

        Node phys = Node::mapping();
        phys["dynamic_friction"] = make_float(0.6);
        phys["static_friction"] = make_float(0.7);
        phys["restitution"] = make_float(0.1);
        write_if_missing(p.assets() / "physics_materials" / "default.yaml", phys);

        write_if_missing(p.assets() / "scenes" / "main" / "scene.yaml", default_scene_node("main"));
        return Project(root);
    }

    /**
     * @brief The engine's assets/config.yaml text with `window.title` set to `title` and
     *        `scene.default_scene` pointing at the new project's main scene; every other line
     *        (settings and comments) verbatim.
     */
    static std::string default_config_text(const std::string& title) {
        std::ifstream in(fs::path(ROOT_DIR) / "assets" / "config.yaml", std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto set_line = [&](const std::string& section, const std::string& key, const std::string& value) {
            const size_t sec = text.find("\n" + section + ":");
            const size_t from = text.rfind(section + ":", 0) == 0 ? 0 : sec;
            if (from == std::string::npos) { text += "\n" + section + ":\n  " + key + ": " + value + "\n"; return; }
            const size_t at = text.find("\n  " + key + ":", from);
            if (at == std::string::npos) return;
            const size_t eol = text.find('\n', at + 1);
            text.replace(at + 1, (eol == std::string::npos ? text.size() : eol) - at - 1, "  " + key + ": " + value);
        };
        std::string quoted = "\"";
        for (char c : title) { if (c == '"' || c == '\\') quoted += '\\'; quoted += c; }
        quoted += "\"";
        set_line("window", "title", quoted);
        set_line("scene", "default_scene", "\"assets/scenes/main/scene.yaml\"");
        return text;
    }

    /** @brief A starter scene document: camera, sun, ground plane, a cube -- named in snake_case,
     *         like everything in assets/. */
    static Node default_scene_node(const std::string& name) {
        auto obj = [](const std::string& n, glm::vec3 pos, glm::vec3 rot, glm::vec3 scl) {
            Node o = Node::mapping();
            o["name"] = Node(n);
            o["active"] = Node(true);
            Node comps = Node::sequence();
            Node t = Node::mapping();
            t["type"] = Node(std::string("Transform"));
            t["position"] = make_vec3(pos);
            t["rotation"] = make_vec3(rot);
            t["scale"] = make_vec3(scl);
            comps.as_seq().push_back(t);
            o["components"] = comps;
            o["children"] = Node::sequence();
            return o;
        };
        Node roots = Node::sequence();
        Node cam = obj("camera", {6.0f, -6.0f, 4.5f}, {63.0f, 0.0f, 45.0f}, glm::vec3(1.0f));
        Node c = Node::mapping();
        c["type"] = Node(std::string("Camera"));
        c["main"] = Node(true);
        c["projection"] = Node(std::string("Perspective"));
        c["fov"] = make_float(50.0);
        cam["components"].as_seq().push_back(c);
        roots.as_seq().push_back(cam);

        Node sun = obj("sun", glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
        Node l = Node::mapping();
        l["type"] = Node(std::string("DirectionalLight"));
        l["direction"] = make_vec3({-0.35f, -0.45f, -0.82f});
        l["color"] = make_color({1.0f, 0.97f, 0.9f});
        l["intensity"] = make_float(1.0);
        l["cast_shadows"] = Node(true);
        sun["components"].as_seq().push_back(l);
        roots.as_seq().push_back(sun);

        Node ground = obj("ground", glm::vec3(0.0f), glm::vec3(0.0f), {8.0f, 8.0f, 1.0f});
        Node gm = Node::mapping();
        gm["type"] = Node(std::string("MeshRenderer"));
        gm["mesh_path"] = Node(std::string("plane"));
        gm["material"] = Node(std::string("materials/default"));
        ground["components"].as_seq().push_back(gm);
        roots.as_seq().push_back(ground);

        Node cube = obj("cube", {0.0f, 0.0f, 0.5f}, glm::vec3(0.0f), glm::vec3(1.0f));
        Node cm = Node::mapping();
        cm["type"] = Node(std::string("MeshRenderer"));
        cm["mesh_path"] = Node(std::string("cube"));
        Node inline_mat = Node::mapping();
        inline_mat["albedo"] = make_color({0.85f, 0.45f, 0.25f});
        inline_mat["roughness"] = make_float(0.5);
        cm["material"] = inline_mat;
        cube["components"].as_seq().push_back(cm);
        roots.as_seq().push_back(cube);

        Node scene = Node::mapping();
        scene["scene_name"] = Node(name);
        scene["root_objects"] = roots;
        Node doc = Node::mapping();
        doc["format"] = Node(std::string("toyengine"));
        doc["scene"] = scene;
        return doc;
    }

private:
    fs::path root_;
    std::map<std::string, std::vector<std::string>> cache_;
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_PROJECT_H
