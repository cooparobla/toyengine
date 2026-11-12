/**
 * @file hub_core.h
 * @brief The project hub's model, with no window or device: the project list, the hub's
 *        settings, and background shell tasks.
 *
 * The hub is a front-end over tools/toyhub -- creating, adding, building, re-pinning and
 * opening a project all run that script (as a Task, its output streamed into the hub's log),
 * so the CLI and the GUI can never disagree about what a project is. Only the cheap,
 * synchronous parts are native here: reading the project list and settings, and forgetting
 * a project.
 *
 * Everything the hub keeps lives in ~/.toyengine/ (shared with tools/toyhub):
 *   projects.yaml   `projects:` -- the absolute path of every project the hub lists. Only
 *                   `new` and `add` put a project here; nothing is auto-detected.
 *   settings.yaml   the hub's own settings (`theme`; default blender_dark)
 *
 * A project's name is always its folder's name. The engine checkout itself is never a project.
 *
 * A project gets its engine in one of three ways, which `HubProject::mode()` names and which the
 * New / Add dialog and a project row's menu both offer: `linked` (a checkout shared with every
 * other linked project), `worktree` (its own git worktree of a checkout, inside the project, on
 * its own branch) and `own` (its own clone, inside the project). `toyhub link|worktree|own`
 * switches a project between them; see tools/toyhub's header for what each costs.
 */

#ifndef TOYENGINE_HUB_HUB_CORE_H
#define TOYENGINE_HUB_HUB_CORE_H

#include "../editor/app/project.h"
#include "../editor/core/process.h"

#include <root_directory.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>

namespace toy::hub {

namespace fs = std::filesystem;
using editor::Node;
using editor::Project;
using editor::get_string;
using editor::Task;
using editor::shell_quote;
using editor::launch_detached;
using editor::command_output;

/** @brief One project row: what the hub shows and what its actions need. */
struct HubProject {
    fs::path root;
    std::string name;           ///< The folder name.
    fs::path toy_file;
    std::string target;         ///< `.toy` target: build/<target>, build/<target>_editor.
    std::string engine_ref;     ///< `.toy` engine.ref
    std::string engine_link;    ///< `.toy` engine.link (absolute: a shared checkout; relative: in the project)
    std::string engine_mode;    ///< `.toy` engine.mode (empty in a `.toy` that predates it: read from the link)
    std::string engine_branch;  ///< `.toy` engine.branch: the engine branch this project's engine work goes on
    fs::file_time_type modified{};

    /**
     * @brief Which of the three ways this project gets its engine: `linked` (a checkout shared
     *        with every other linked project), `worktree` (its own git worktree of a checkout,
     *        inside the project, on its own branch) or `own` (its own clone, inside the
     *        project). `pinned` is a `.toy` with no link at all, whose engine is a clone in
     *        `.libs/toyengine`.
     */
    std::string mode() const {
        if (!engine_mode.empty()) return engine_mode;
        if (engine_link.empty()) return "pinned";
        return engine_link.front() == '/' ? "linked" : "own";
    }
    /** @brief True when the engine is a checkout shared with every other linked project. */
    bool linked() const { return mode() == "linked"; }
    /** @brief True when the engine lives inside the project (a worktree or a clone of its own). */
    bool own_engine() const { const std::string m = mode(); return m == "worktree" || m == "own"; }

    fs::path editor_binary() const { return root / "build" / (target + "_editor"); }
    fs::path game_binary() const { return root / "build" / target; }
    bool built() const { std::error_code ec; return fs::exists(editor_binary(), ec); }
    /** @brief The engine mode, or a short engine ref for a pinned project. */
    std::string engine_label() const {
        const std::string m = mode();
        if (m != "pinned") return m;
        if (engine_ref.empty()) return "unpinned";
        return engine_ref.size() > 12 && engine_ref.find_first_not_of("0123456789abcdef") == std::string::npos
                   ? engine_ref.substr(0, 10) : engine_ref;
    }
    /** @brief What the engine badge explains on hover. */
    std::string engine_tooltip() const {
        const std::string m = mode();
        if (m == "linked")
            return "Linked to " + engine_link + "\nBuilds that working tree, uncommitted edits included -- and so does "
                   "every other linked project on this machine.";
        if (m == "worktree")
            return "Its own engine worktree in " + engine_link + (engine_branch.empty() ? "" : ", on branch " + engine_branch) +
                   "\nEngine work is committed on that branch, in the engine's own history; no other project sees it.";
        if (m == "own")
            return "Its own engine clone in " + engine_link + "\nIndependent of every toyengine checkout on this machine.";
        return "Pinned engine commit " + engine_ref + "\n(.libs/toyengine is a clone checked out there)";
    }
};

/** @brief The tools/toyhub script of the engine checkout this hub was built from. */
inline fs::path toyhub_script() { return fs::path(ROOT_DIR) / "tools" / "toyhub"; }

/** @brief ~/.toyengine: the hub's settings and project list. */
inline fs::path hub_home() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".toyengine";
}
inline fs::path projects_file() { return hub_home() / "projects.yaml"; }
inline fs::path settings_file() { return hub_home() / "settings.yaml"; }

/** @brief The hub's settings (an empty mapping if none yet). */
inline Node load_settings() {
    try { if (auto n = coopa::yaml::try_load_document(settings_file())) if (n->is_mapping()) return *n; } catch (...) {}
    return Node::mapping();
}
inline void save_settings(const Node& settings) {
    std::error_code ec;
    fs::create_directories(hub_home(), ec);
    try { coopa::yaml::save_document(settings_file(), settings); } catch (...) {}
}

/** @brief The theme the hub starts with when its settings name none. */
inline const char* default_hub_theme() { return "blender_dark"; }

inline std::string normalized(const fs::path& p) {
    std::string s = fs::absolute(p).lexically_normal().string();
    while (s.size() > 1 && s.back() == '/') s.pop_back();   // "/x/" and "/x" are one project
    return s;
}

/** @brief True for the engine checkout this hub belongs to, which is never a project. */
inline bool is_engine_checkout(const fs::path& p) {
    std::error_code ec;
    return fs::equivalent(p, fs::path(ROOT_DIR), ec);
}

/** @brief The listed project paths, in file order. */
inline std::vector<std::string> read_project_list() {
    std::vector<std::string> out;
    try {
        if (auto doc = coopa::yaml::try_load_document(projects_file())) {
            if (doc->is_mapping() && doc->contains("projects") && doc->at("projects").is_sequence()) {
                for (const auto& n : doc->at("projects").as_seq()) if (n.is_string()) out.push_back(n.get_value<std::string>());
            }
        }
    } catch (...) {}
    return out;
}

/** @brief Writes the list in the block form tools/toyhub reads and writes too. */
inline void write_project_list(const std::vector<std::string>& paths) {
    std::error_code ec;
    fs::create_directories(hub_home(), ec);
    std::ofstream out(projects_file(), std::ios::trunc);
    out << "# Projects listed by the toyengine hub and tools/toyhub (add / new / forget).\nprojects:\n";
    for (const auto& p : paths) {
        std::string q;
        for (char c : p) { if (c == '"' || c == '\\') q += '\\'; q += c; }
        out << "  - \"" << q << "\"\n";
    }
}

/** @brief Lists `root` (no-op if listed, or if it is the engine checkout). The hub itself adds
 *         through `toyhub add`, which also sets a bare folder up; this is the bookkeeping half. */
inline bool list_project(const fs::path& root) {
    if (is_engine_checkout(root)) return false;
    auto list = read_project_list();
    const std::string s = normalized(root);
    if (std::find(list.begin(), list.end(), s) == list.end()) list.push_back(s);
    write_project_list(list);
    return true;
}

/** @brief Removes `root` from the list (its files stay). */
inline void forget_project(const fs::path& root) {
    const std::string s = normalized(root);
    auto list = read_project_list();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::string& p) { return normalized(p) == s; }), list.end());
    write_project_list(list);
}

/** @brief Reads one project directory (nullopt unless it has a .toy and isn't the engine). */
inline std::optional<HubProject> read_project(const fs::path& root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec) || is_engine_checkout(root)) return std::nullopt;
    const Project p(root);
    HubProject h;
    h.root = fs::path(normalized(root));
    h.name = h.root.filename().string();
    h.toy_file = p.project_file();
    if (h.toy_file.empty()) return std::nullopt;
    const Node toy = Project::load_toy(h.toy_file);
    h.target = get_string(toy, "target");
    if (h.target.empty()) h.target = h.toy_file.stem().string();
    if (toy.contains("engine") && toy.at("engine").is_mapping()) {
        h.engine_ref = get_string(toy.at("engine"), "ref");
        h.engine_link = get_string(toy.at("engine"), "link");
        h.engine_mode = get_string(toy.at("engine"), "mode");
        h.engine_branch = get_string(toy.at("engine"), "branch");
    }
    h.modified = fs::last_write_time(h.toy_file, ec);
    if (fs::is_directory(p.assets(), ec)) {
        // assets/ itself changes when a file is added; good enough for "last edited".
        h.modified = std::max(h.modified, fs::last_write_time(p.assets(), ec));
    }
    return h;
}

/** @brief Every listed project that still exists, newest first. */
inline std::vector<HubProject> load_projects() {
    std::set<std::string> seen;
    std::vector<HubProject> out;
    for (const auto& p : read_project_list()) {
        if (!seen.insert(normalized(p)).second) continue;
        if (auto h = read_project(p)) out.push_back(std::move(*h));
    }
    std::stable_sort(out.begin(), out.end(), [](const HubProject& a, const HubProject& b) { return a.modified > b.modified; });
    return out;
}

/** @brief "just now", "5 minutes ago", "3 days ago". */
inline std::string relative_time(fs::file_time_type t) {
    if (t == fs::file_time_type{}) return "";
    const auto age = fs::file_time_type::clock::now() - t;
    const long s = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(age).count());
    auto plural = [](long n, const char* unit) { return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago"; };
    if (s < 60) return "just now";
    if (s < 3600) return plural(s / 60, "minute");
    if (s < 86400) return plural(s / 3600, "hour");
    if (s < 86400 * 60) return plural(s / 86400, "day");
    return plural(s / (86400 * 30), "month");
}

/** @brief `toyhub <args...>` as a shell command line. */
inline std::string toyhub_command(const std::vector<std::string>& args) {
    std::string cmd = shell_quote(toyhub_script().string());
    for (const auto& a : args) cmd += " " + shell_quote(a);
    return cmd;
}

/** @brief Opens a folder in the platform file browser. */
inline void reveal_in_file_browser(const fs::path& p) { editor::open_with_system(p); }

/** @brief True for a toyengine checkout a project can link to (its CMake + the engine headers). */
inline bool is_toyengine_checkout(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p / "CMakeLists.txt", ec) && fs::exists(p / "toyengine" / "core" / "engine.h", ec) &&
           fs::exists(p / "cmake" / "ToyProject.cmake", ec);
}

} // namespace toy::hub

#endif // TOYENGINE_HUB_HUB_CORE_H
