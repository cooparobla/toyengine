#include "editor/app/project.h"

namespace toy {
namespace editor {

std::string strip_yaml_ext(const std::string& rel) {
    const std::string e = fs::path(rel).extension().string();
    return (e == ".yaml" || e == ".caml") ? rel.substr(0, rel.size() - e.size()) : rel;
}

std::string asset_type_dir(const std::string& rel) {
    const std::string g = fs::path(rel).generic_string();
    if (g.rfind("ui/themes/", 0) == 0) return "ui/themes";
    const size_t slash = g.find('/');
    return slash == std::string::npos ? std::string() : g.substr(0, slash);
}

bool is_scene_folder_file(const std::string& rel) {
    const fs::path p(rel);
    const std::string g = p.generic_string();
    return g.rfind("scenes/", 0) == 0 && p.filename().string().rfind("scene", 0) == 0 &&
           p.parent_path().generic_string() != "scenes" && p.parent_path().filename() != "meshes";
}

bool is_scene_local_file(const std::string& rel) {
    const std::string g = fs::path(rel).generic_string();
    return g.rfind("scenes/", 0) == 0 && g.find("/meshes/") != std::string::npos;
}

std::vector<std::string> asset_tags(const std::string& rel) {
    std::vector<std::string> out;
    if (is_scene_local_file(rel)) return out;
    const std::string type = asset_type_dir(rel);
    if (type.empty()) return out;
    fs::path dir = fs::path(fs::path(rel).generic_string().substr(type.size() + 1)).parent_path();
    if (is_scene_folder_file(rel)) dir = dir.parent_path();
    for (const auto& part : dir) {
        const std::string s = part.generic_string();
        if (!s.empty() && s != ".") out.push_back(s);
    }
    return out;
}

std::string with_asset_tags(const std::string& rel, const std::vector<std::string>& tags) {
    const std::string type = asset_type_dir(rel);
    if (type.empty() || is_scene_local_file(rel)) return rel;
    const fs::path p(rel);
    fs::path out(type);
    for (const auto& t : tags) out /= t;
    if (is_scene_folder_file(rel)) out /= p.parent_path().filename();
    return (out / p.filename()).generic_string();
}

std::string short_ref(const std::string& rel) {
    const std::string type = asset_type_dir(rel);
    if (type.empty() || is_scene_local_file(rel)) return fs::path(rel).generic_string();
    const fs::path p(rel);
    if (is_scene_folder_file(rel)) return "scenes/" + p.parent_path().filename().generic_string() + "/" + p.filename().generic_string();
    return type + "/" + p.filename().generic_string();
}

RefRename RefRename::between(const std::string& from, const std::string& to) {
    RefRename r;
    const std::string f = fs::path(from).generic_string(), t = fs::path(to).generic_string();
    // First rule for a value wins -- even a no-op one: the short form goes first, so moving an
    // untagged asset (whose full path IS its short form) leaves its short references alone.
    std::vector<std::string> seen;
    auto add = [&seen](std::vector<std::pair<std::string, std::string>>& v, const std::string& a, const std::string& b) {
        if (std::find(seen.begin(), seen.end(), a) != seen.end()) return;
        seen.push_back(a);
        if (a != b) v.push_back({a, b});
    };
    if (f.find("meshes/") != std::string::npos) {
        r.mesh_keys = {"mesh_path", "side_mesh", "mesh"};
        auto under_meshes = [](const std::string& g) {
            const std::string s = strip_yaml_ext(g);
            if (s.rfind("meshes/", 0) == 0) return s.substr(7);
            const size_t at = s.find("/meshes/");
            return at == std::string::npos ? fs::path(s).filename().string() : s.substr(at + 8);
        };
        add(r.meshes, mesh_ref(f), mesh_ref(t));
        add(r.meshes, under_meshes(f), under_meshes(t));
        return r;
    }
    for (const auto& [a, b] : {std::pair{short_ref(f), short_ref(t)}, std::pair{f, t}}) {
        add(r.paths, a, b);
        add(r.paths, "assets/" + a, "assets/" + b);   // config.yaml's root-relative refs
        add(r.paths, strip_yaml_ext(a), strip_yaml_ext(b));
    }
    return r;
}

std::vector<std::string> RefRename::needles() const {
    std::vector<std::string> out;
    for (const auto& p : paths) out.push_back(p.first);
    for (const auto& m : meshes) out.push_back(m.first);
    return out;
}

int RefRename::apply_(Node& n, const std::string* key) const {
    int changed = 0;
    if (n.is_string()) {
        const std::string v = n.get_value<std::string>();
        for (const auto& p : paths) {
            if (v == p.first) { n = Node(p.second); return 1; }
        }
        if (key && std::find(mesh_keys.begin(), mesh_keys.end(), *key) != mesh_keys.end()) {
            for (const auto& m : meshes) {
                if (v == m.first) { n = Node(m.second); return 1; }
            }
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

bool Project::valid() const { std::error_code ec; return fs::is_directory(assets(), ec) || !project_file().empty(); }

bool Project::has_assets() const { std::error_code ec; return fs::is_directory(assets(), ec); }

std::string Project::name() const {
    const fs::path r = root_.has_filename() ? root_ : root_.parent_path();   // "/x/y/" -> "y"
    return r.filename().string();
}

fs::path Project::find_project_file(const fs::path& root) {
    std::error_code ec;
    std::vector<fs::path> found;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".toy") found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    return found.empty() ? fs::path() : found.front();
}

Node Project::load_toy(const fs::path& path) {
    try { if (auto n = coopa::yaml::try_load_document(path)) return *n; } catch (...) {}
    return Node::mapping();
}

Node Project::default_toy() {
    Node toy = Node::mapping();
    toy["format"] = Node(std::string("toyproject"));
    toy["version"] = Node(int64_t(1));
    return toy;
}

std::string Project::relative(const fs::path& p) const {
    auto under = [&](const fs::path& base) -> std::string {
        std::error_code ec;
        const std::string s = fs::relative(p, base, ec).generic_string();
        return ec || s.empty() || s.rfind("..", 0) == 0 ? std::string() : s;
    };
    if (std::string s = under(assets()); !s.empty()) return s;
    if (!is_engine()) if (std::string s = under(engine_assets()); !s.empty()) return s;
    return p.generic_string();
}

fs::path Project::absolute(const std::string& rel) const {
    const fs::path p(rel);
    if (p.is_absolute()) return p;
    const fs::path mine = assets() / p;
    if (exists_(mine)) return mine;
    const fs::path theirs = engine_assets() / p;
    if (!is_engine() && exists_(theirs)) return theirs;
    // By name, wherever its tag folders put it (`materials/brick` -> materials/metal/brick.yaml).
    if (auto found = coopa::asset::AssetIndex::find(assets(), rel)) return *found;
    if (!is_engine()) if (auto found = coopa::asset::AssetIndex::find(engine_assets(), rel)) return *found;
    return mine;
}

bool Project::is_engine() const {
    std::error_code ec;
    return fs::equivalent(root_, fs::path(ROOT_DIR), ec);
}

bool Project::is_engine_path(const fs::path& abs) const {
    if (is_engine() || abs.empty()) return false;
    const std::string s = fs::path(abs).lexically_normal().generic_string();
    const std::string base = engine_assets().lexically_normal().generic_string() + "/";
    return s.rfind(base, 0) == 0;
}

const std::vector<std::string>& Project::list_engine(const std::string& dir, const std::string& ext) {
    static const std::vector<std::string> none;
    if (is_engine()) return none;
    const std::string key = "engine|" + dir + "|" + ext;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    // Shadowed by NAME, not path: the project's materials/brick.yaml is what `materials/brick`
    // resolves to even when toyengine's sits in a tag folder (materials/building/brick.yaml).
    std::vector<std::string> mine;
    for (const auto& rel : list(dir, ext)) mine.push_back(short_ref(rel));
    std::sort(mine.begin(), mine.end());
    std::vector<std::string> out;
    for (const auto& rel : scan_(engine_assets(), dir, ext)) {
        if (!std::binary_search(mine.begin(), mine.end(), short_ref(rel))) out.push_back(rel);
    }
    return cache_[key] = out;
}

const std::vector<std::string>& Project::list(const std::string& dir, const std::string& ext) {
    const std::string key = dir + "|" + ext;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    return cache_[key] = scan_(assets(), dir, ext);
}

void Project::refresh() { cache_.clear(); coopa::asset::AssetIndex::invalidate(); }

std::vector<std::string> Project::rename_asset(const std::string& from, const std::string& to) {
    const fs::path src = absolute(from), dst = assets() / to;
    std::error_code ec;
    std::vector<RefRename> renames;
    const bool scene_dir = is_scene_folder_file(from) && is_scene_folder_file(to) &&
                           fs::path(from).parent_path() != fs::path(to).parent_path();
    if (scene_dir) {
        // The folder is the scene: move it, then (if the file name changed too) the file.
        const fs::path sdir = src.parent_path(), ddir = dst.parent_path();
        if (fs::exists(ddir, ec)) throw std::runtime_error(relative(ddir) + " already exists");
        fs::create_directories(ddir.parent_path());
        fs::rename(sdir, ddir);
        if (src.filename() != dst.filename()) fs::rename(ddir / src.filename(), dst);
        const std::string fdir = fs::path(from).parent_path().generic_string(), tdir = fs::path(to).parent_path().generic_string();
        for (auto e = fs::recursive_directory_iterator(ddir, ec); !ec && e != fs::recursive_directory_iterator(); e.increment(ec)) {
            if (!e->is_regular_file()) continue;
            const std::string inner = fs::relative(e->path(), ddir, ec).generic_string();
            if (inner.find("meshes/") != std::string::npos) continue;   // scene-local: found from the scene's folder
            const std::string was = fdir + "/" + (e->path().filename() == dst.filename() ? src.filename().generic_string() : inner);
            renames.push_back(RefRename::between(was, tdir + "/" + inner));
        }
    } else {
        if (fs::exists(dst, ec) && !fs::equivalent(src, dst, ec)) throw std::runtime_error(to + " already exists");
        fs::create_directories(dst.parent_path());
        fs::rename(src, dst);
        if (from.find("meshes/") != std::string::npos) {
            const fs::path lod_src = src.parent_path() / (src.stem().string() + ".lod" + src.extension().string());
            if (fs::exists(lod_src, ec)) {
                fs::rename(lod_src, dst.parent_path() / (dst.stem().string() + ".lod" + dst.extension().string()), ec);
            }
        }
        renames.push_back(RefRename::between(from, to));
    }
    coopa::asset::AssetIndex::invalidate();
    renames.erase(std::remove_if(renames.begin(), renames.end(), [](const RefRename& r) { return r.empty(); }), renames.end());
    std::vector<std::string> rewritten;
    if (renames.empty()) { refresh(); return rewritten; }
    std::vector<std::string> needles;
    bool mesh = false;
    for (const auto& r : renames) {
        for (const auto& n : r.needles()) needles.push_back(n);
        mesh |= !r.meshes.empty();
    }
    // A scene-local mesh (scenes/<s>/meshes/x.yaml) is only visible to its own scene's files;
    // a global one is shadowed in any directory that has its own meshes/<name>.yaml.
    const std::string g = fs::path(from).generic_string();
    const bool local_mesh = mesh && g.rfind("meshes/", 0) != 0;
    const fs::path scan_root = local_mesh ? assets() / g.substr(0, g.find("/meshes/")) : assets();
    const std::string mesh_name = mesh ? mesh_ref(from) : std::string();
    for (auto e = fs::recursive_directory_iterator(scan_root, ec); e != fs::recursive_directory_iterator(); e.increment(ec)) {
        if (ec) break;
        if (!e->is_regular_file() || e->path().extension() != ".yaml") continue;   // .caml is a packaged copy
        if (mesh && !local_mesh && e->path().parent_path() != assets() &&
            fs::exists(e->path().parent_path() / "meshes" / (mesh_name + ".yaml"), ec)) continue;
        // Mesh files are large and only refer to other meshes in `lods:`.
        const bool in_meshes = fs::path(relative(e->path())).generic_string().rfind("meshes/", 0) == 0 ||
                               e->path().parent_path().filename() == "meshes";
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
            int changed = 0;
            for (const auto& r : renames) changed += r.apply(doc);
            if (changed == 0) continue;
            coopa::yaml::save_document(e->path(), doc);
            rewritten.push_back(relative(e->path()));
        } catch (...) {
            // An unparsable file is left alone; the engine can't load it either.
        }
    }
    refresh();
    return rewritten;
}

fs::path Project::prefs_path() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".toyengine_editor.yaml";
}

Node Project::load_prefs() {
    try { if (auto n = coopa::yaml::try_load_document(prefs_path())) return *n; } catch (...) {}
    return Node::mapping();
}

std::vector<std::string> Project::recent_projects() {
    std::vector<std::string> out;
    const Node prefs = load_prefs();
    if (prefs.contains("recent_projects")) {
        for (const auto& r : prefs.at("recent_projects").as_seq()) if (r.is_string()) out.push_back(r.get_value<std::string>());
    }
    return out;
}

void Project::remember(const fs::path& root) {
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

Project Project::create(const fs::path& root) {
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
    // config.yaml: the engine's own (ROOT_DIR/assets/config.yaml) -- only the window title and
    // the default scene are this project's. Paths it names that the project lacks (the palette)
    // resolve to the engine's assets/.
    if (!fs::exists(p.config_path(), ec)) {
        coopa::yaml::save_document(p.config_path(), default_config(p.name().empty() ? std::string("toyengine") : p.name()));
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

Node Project::default_config(const std::string& title) {
    Node cfg = Node::mapping();
    try {
        if (auto n = coopa::yaml::try_load_document(fs::path(ROOT_DIR) / "assets" / "config.yaml"); n && n->is_mapping()) cfg = *n;
    } catch (...) {}
    auto section = [&](const char* name) -> Node& {
        if (!cfg.contains(name) || !cfg[name].is_mapping()) cfg[name] = Node::mapping();
        return cfg[name];
    };
    section("window")["title"] = Node(title);
    section("scene")["default_scene"] = Node(std::string("assets/scenes/main/scene.yaml"));
    // A game should not write a screenshot every time it quits (the engine repo's own config
    // keeps it on: its tests and capture workflow rely on it).
    section("output")["save_on_exit"] = Node(false);
    return cfg;
}

Node Project::default_scene_node(const std::string& name) {
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
    doc["scene"] = scene;
    return doc;
}

std::vector<std::string> Project::scan_(const fs::path& root, const std::string& dir, const std::string& ext) {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path base = dir.empty() ? root : root / dir;
    if (fs::is_directory(base, ec)) {
        for (auto e = fs::recursive_directory_iterator(base, ec); e != fs::recursive_directory_iterator(); e.increment(ec)) {
            if (ec) break;
            if (!e->is_regular_file()) continue;
            const std::string fe = e->path().extension().string();
            const std::string fname = e->path().filename().string();
            if (fname.find(".lod.") != std::string::npos) continue;   // sidecars aren't meshes
            if (!ext.empty() && fe != ext && !(ext == ".yaml" && fe == ".caml")) continue;
            out.push_back(fs::relative(e->path(), root, ec).generic_string());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> Project::scene_files_(const std::vector<std::string>& files) {
    std::vector<std::string> out;
    for (const auto& p : files) {
        const std::string f = fs::path(p).filename().string();
        if (fs::path(p).parent_path().filename() == "meshes") continue;
        if (f.rfind("scene", 0) == 0 || fs::path(p).parent_path() == "scenes") out.push_back(p);
    }
    return out;
}

bool Project::exists_(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) || coopa::yaml::document_exists(p);
}

} // namespace editor
} // namespace toy
