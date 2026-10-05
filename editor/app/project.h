/**
 * @file project.h
 * @brief A toyengine project on disk: a directory whose assets/ folder the editor builds.
 *
 * Layout the editor reads and writes -- the same one the game loads:
 * @code
 * <root>/assets/config.yaml
 *              scenes/<name>/scene.yaml   (+ scene-local meshes/, textures/)
 *              meshes/*.yaml              (+ <mesh>.lod.yaml sidecars)
 *              materials/*.yaml
 *              physics_materials/*.yaml
 *              textures/*.png
 * @endcode
 * Shaders and fonts come from the engine build, not the project.
 */

#ifndef TOYEDITOR_APP_PROJECT_H
#define TOYEDITOR_APP_PROJECT_H

#include "../core/yaml_util.h"
#include "../mesh/edit_mesh.h"
#include "../mesh/primitives.h"

#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace toy::editor {

namespace fs = std::filesystem;

class Project {
public:
    Project() = default;
    explicit Project(fs::path root) : root_(std::move(root)) {}

    const fs::path& root() const { return root_; }
    fs::path assets() const { return root_ / "assets"; }
    fs::path config_path() const { return assets() / "config.yaml"; }
    bool valid() const { std::error_code ec; return fs::is_directory(assets(), ec); }
    std::string name() const { return root_.filename().string(); }

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
     * @brief Creates a new project skeleton at `root`: folders, a config.yaml, starter
     *        meshes and a default scene with a camera, a sun and a ground plane.
     * @return The project. Existing files are never overwritten.
     */
    static Project create(const fs::path& root) {
        Project p(root);
        std::error_code ec;
        for (const char* d : {"scenes/main", "objects", "meshes", "materials", "physics_materials", "textures", "ui"}) {
            fs::create_directories(p.assets() / d, ec);
        }
        auto write_if_missing = [&](const fs::path& path, const Node& node) {
            if (!fs::exists(path, ec)) coopa::yaml::save_document(path, node);
        };
        // config.yaml: the handful of keys a fresh project wants explicit; everything else
        // falls back to engine defaults / quality presets (so nothing gets pinned).
        Node cfg = Node::mapping();
        Node win = Node::mapping();
        win["title"] = Node(p.name().empty() ? std::string("toyengine") : p.name());
        win["width"] = Node(int64_t(1280));
        win["height"] = Node(int64_t(720));
        win["vsync"] = Node(true);
        cfg["window"] = win;
        Node sc = Node::mapping();
        sc["default_scene"] = Node(std::string("assets/scenes/main/scene.yaml"));
        cfg["scene"] = sc;
        Node render = Node::mapping();
        render["resolution_mode"] = Node(std::string("fixed"));
        render["render_width"] = Node(int64_t(480));
        render["render_height"] = Node(int64_t(270));
        render["shadows_enabled"] = Node(true);
        render["aa_mode"] = Node(std::string("off"));
        render["transparency_enabled"] = Node(true);   // BLEND materials render (engine default: off)
        cfg["render"] = render;
        write_if_missing(p.config_path(), cfg);

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

        write_if_missing(p.assets() / "scenes" / "main" / "scene.yaml", default_scene_node("Main"));
        return p;
    }

    /** @brief A starter scene document: camera, sun, ground plane, a cube. */
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
        Node cam = obj("Camera", {6.0f, -6.0f, 4.5f}, {63.0f, 0.0f, 45.0f}, glm::vec3(1.0f));
        Node c = Node::mapping();
        c["type"] = Node(std::string("Camera"));
        c["main"] = Node(true);
        c["projection"] = Node(std::string("Perspective"));
        c["fov"] = make_float(50.0);
        cam["components"].as_seq().push_back(c);
        roots.as_seq().push_back(cam);

        Node sun = obj("Sun", glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
        Node l = Node::mapping();
        l["type"] = Node(std::string("DirectionalLight"));
        l["direction"] = make_vec3({-0.35f, -0.45f, -0.82f});
        l["color"] = make_color({1.0f, 0.97f, 0.9f});
        l["intensity"] = make_float(1.0);
        l["cast_shadows"] = Node(true);
        sun["components"].as_seq().push_back(l);
        roots.as_seq().push_back(sun);

        Node ground = obj("Ground", glm::vec3(0.0f), glm::vec3(0.0f), {8.0f, 8.0f, 1.0f});
        Node gm = Node::mapping();
        gm["type"] = Node(std::string("MeshRenderer"));
        gm["mesh_path"] = Node(std::string("plane"));
        gm["material"] = Node(std::string("materials/default"));
        ground["components"].as_seq().push_back(gm);
        roots.as_seq().push_back(ground);

        Node cube = obj("Cube", {0.0f, 0.0f, 0.5f}, glm::vec3(0.0f), glm::vec3(1.0f));
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
