#include <toyengine/ui/ui_tunables.h>

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>

#include <fkYAML/node.hpp>

#include <coopa/yaml/document.h>
// Before font_defaults.h: it reaches uicoopa/widgets/graphic.h, which dereferences a
// SceneObject it only forward-declares. Every other TU happens to include the scene first.
#include <coopa/scene/scene_object.h>
#include <uicoopa/text/font_defaults.h>

namespace toy::ui {
namespace {

using coopa::ui::imm::Anchor;

struct AnchorName {
    Anchor a;
    const char* name;
};

/// The single list: anchor_name(), parse_anchor() and anchor_names() all walk it, so a value
/// cannot be spellable one way and parseable another.
constexpr AnchorName k_anchors[] = {
    {Anchor::TopLeft, "TopLeft"},         {Anchor::TopCenter, "TopCenter"},
    {Anchor::TopRight, "TopRight"},       {Anchor::MiddleLeft, "MiddleLeft"},
    {Anchor::Center, "Center"},           {Anchor::MiddleRight, "MiddleRight"},
    {Anchor::BottomLeft, "BottomLeft"},   {Anchor::BottomCenter, "BottomCenter"},
    {Anchor::BottomRight, "BottomRight"},
};

/// A [x, y] pair or {x: , y: } map, the two spellings every other coopa file accepts.
bool read_vec2(const fkyaml::node& n, glm::vec2& out) {
    auto num = [](const fkyaml::node& v) -> float {
        if (v.is_integer()) return static_cast<float>(v.get_value<int64_t>());
        if (v.is_float_number()) return static_cast<float>(v.get_value<double>());
        return 0.0f;
    };
    if (n.is_sequence() && n.size() >= 2) {
        out = glm::vec2(num(n[0]), num(n[1]));
        return true;
    }
    if (n.is_mapping() && n.contains("x") && n.contains("y")) {
        out = glm::vec2(num(n.at("x")), num(n.at("y")));
        return true;
    }
    return false;
}

std::string resolve_path(const std::string& ref) {
    if (ref.empty()) return {};
    const bool has_suffix = ref.size() > 10 && ref.rfind(".tune.yaml") == ref.size() - 10;
    const std::string rel = has_suffix ? ref : ref + ".tune.yaml";
    for (const std::string& root : coopa::ui::FontDefaults::search_dirs) {
        const std::filesystem::path p = std::filesystem::path(root) / rel;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return p.string();
    }
    // Not found: name where it WOULD go, under the first root, so save() can create it.
    if (!coopa::ui::FontDefaults::search_dirs.empty()) {
        return (std::filesystem::path(coopa::ui::FontDefaults::search_dirs.front()) / rel).string();
    }
    return rel;
}

}  // namespace

std::string tune_file_stem(std::string_view type_name) {
    std::string out;
    out.reserve(type_name.size() + 4);
    for (size_t i = 0; i < type_name.size(); ++i) {
        const char c = type_name[i];
        if (c >= 'A' && c <= 'Z') {
            // A boundary is a capital that follows a lower-case letter or digit ("DemoHud" ->
            // "demo_hud"), so a run of capitals stays together ("HUDPanel" -> "hud_panel").
            const bool prev_lower = i > 0 && !(type_name[i - 1] >= 'A' && type_name[i - 1] <= 'Z');
            if (prev_lower) out += '_';
            out += static_cast<char>(c - 'A' + 'a');
        } else {
            out += c;
        }
    }
    return out;
}

const char* anchor_name(Anchor a) {
    for (const AnchorName& e : k_anchors) {
        if (e.a == a) return e.name;
    }
    return "";
}

bool parse_anchor(const std::string& s, Anchor& out) {
    for (const AnchorName& e : k_anchors) {
        if (s == e.name) {
            out = e.a;
            return true;
        }
    }
    return false;
}

const std::vector<std::string>& anchor_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> v;
        for (const AnchorName& e : k_anchors) v.emplace_back(e.name);
        return v;
    }();
    return names;
}

bool Tunables::load(const std::string& ref) {
    declared_.clear();
    file_.clear();
    dirty_ = false;
    path_ = resolve_path(ref);
    if (path_.empty()) return true;

    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) return true;   // no file is the normal case

    try {
        const fkyaml::node root = coopa::yaml::load_document(path_);
        if (!root.is_mapping()) return true;
        for (const auto& [k, v] : root.as_map()) {
            if (!v.is_mapping()) continue;
            Tunable t;
            t.name = k.get_value<std::string>();
            if (v.contains("anchor")) parse_anchor(v.at("anchor").get_value<std::string>(), t.anchor);
            if (v.contains("pos")) read_vec2(v.at("pos"), t.pos);
            if (v.contains("size")) read_vec2(v.at("size"), t.size);
            file_.push_back(t);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[toy::ui] tunables '%s' failed to parse (%s) -- using code defaults\n",
                     path_.c_str(), e.what());
        declared_.clear();
        file_.clear();
        return false;
    }
    return true;
}

const Tunable* Tunables::find(std::string_view name) const {
    auto it = std::find_if(declared_.begin(), declared_.end(),
                           [&](const Tunable& t) { return t.name == name; });
    if (it != declared_.end()) return &*it;
    // Then the file: before the screen has drawn, the file is all that is known about a name,
    // which is what a tool inspecting a freshly loaded set reads.
    auto f = std::find_if(file_.begin(), file_.end(),
                          [&](const Tunable& t) { return t.name == name; });
    return f == file_.end() ? nullptr : &*f;
}

coopa::ui::imm::Box Tunables::resolve(const coopa::ui::imm::Context& ctx, const Tunable& t) const {
    return ctx.anchor(t.anchor, t.pos, t.size);
}

coopa::ui::imm::Box Tunables::box(coopa::ui::imm::Context& ctx, std::string_view name,
                                  Anchor anchor, glm::vec2 pos, glm::vec2 size) {
    // Declared-only lookup on purpose: find() also sees the file, and a file entry must not
    // stop the code's declaration from being recorded.
    auto decl = std::find_if(declared_.begin(), declared_.end(),
                             [&](const Tunable& t) { return t.name == name; });
    const Tunable* existing = decl == declared_.end() ? nullptr : &*decl;
    if (!existing) {
        // First time this screen asked for it: start from the CODE default, then let the file
        // override whichever of anchor/pos/size it stated. Declaring it here is what makes a
        // freshly added panel editable without anyone having to seed a file by hand.
        Tunable t{std::string(name), anchor, pos, size};
        auto it = std::find_if(file_.begin(), file_.end(),
                               [&](const Tunable& f) { return f.name == name; });
        if (it != file_.end()) t = *it;
        declared_.push_back(t);
        existing = &declared_.back();
    }
    return resolve(ctx, *existing);
}

void Tunables::set(std::string_view name, Anchor anchor, glm::vec2 pos, glm::vec2 size) {
    auto it = std::find_if(declared_.begin(), declared_.end(),
                           [&](const Tunable& t) { return t.name == name; });
    if (it == declared_.end()) {
        declared_.push_back(Tunable{std::string(name), anchor, pos, size});
    } else {
        it->anchor = anchor;
        it->pos = pos;
        it->size = size;
    }
    dirty_ = true;
}

void Tunables::forget(std::string_view name) {
    const auto gone = [&](const Tunable& t) { return t.name == name; };
    const bool had = std::any_of(declared_.begin(), declared_.end(), gone) ||
                     std::any_of(file_.begin(), file_.end(), gone);
    declared_.erase(std::remove_if(declared_.begin(), declared_.end(), gone), declared_.end());
    file_.erase(std::remove_if(file_.begin(), file_.end(), gone), file_.end());
    if (had) dirty_ = true;
}

bool Tunables::save() const {
    if (path_.empty()) return false;
    // Nothing declared means the screen has not drawn yet. Writing now would replace a real
    // file with an empty one, so refuse rather than destroy someone's placement.
    if (declared_.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
    std::ofstream out(path_, std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "[toy::ui] could not write tunables '%s'\n", path_.c_str());
        return false;
    }
    out << "# Where this screen's panels sit. Written by the editor's UI workspace (Placement\n"
           "# tab); the screen's own code supplies the default for anything not stated here.\n"
           "# pos is in UI space (+Y down) and moves the box, not the anchor.\n";
    for (const Tunable& t : declared_) {
        out << t.name << ": { anchor: " << anchor_name(t.anchor)
            << ", pos: [" << t.pos.x << ", " << t.pos.y << "]"
            << ", size: [" << t.size.x << ", " << t.size.y << "] }\n";
    }
    dirty_ = false;
    return true;
}

}  // namespace toy::ui
