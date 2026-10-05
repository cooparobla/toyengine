// toyengine editor -- scenes, meshes, materials and render settings for toyengine projects.
//
// Usage:   toyengine_editor [project_dir] [--scene <scene.yaml>] [--new-project <dir>]
//                          [--package <out_dir>]
//          (no project: a game project's editor opens its own project; toyengine's opens the
//           most recent one, else this repository's own assets/)
//          --package: Build > Package from the command line (no window), then exit.
// Build > Refresh (Shift Ctrl B) rebuilds this editor's own build; when that changed the binary,
// "Relaunch Editor" exec()s the new one on the same project.
// Headless smoke run (no visible window):
//          HEADLESS=1 MAX_FRAMES=60 EDITOR_SCREENSHOT=out.png ./build/toyengine_editor
//
// The editor embeds the real toy::core::Engine, so "Full Render" in its viewport is the
// game's own PixelRenderPipeline. A renderer restart (startup-only render settings) or a
// project switch tears the Engine down and builds a new one; open documents survive a
// restart via EditorState.

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <toyengine/core/caml_codec.h>
#include <toyengine/core/config.h>
#include <toyengine/core/engine.h>

#include "app/editor_app.h"
#include "build/packager.h"
#include "core/process.h"

#include <unistd.h>

#include <root_directory.h>

namespace {

namespace fs = std::filesystem;

bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

/** @brief The config the editor's Engine runs with: the project's, adjusted for editing. */
toy::core::AppConfig editor_config(const toy::core::AppConfig& project_config, const toy::editor::Project& project) {
    toy::core::AppConfig cfg = project_config;
    cfg.window.title = "toyengine editor - " + project.name();
    const toy::editor::Node prefs = toy::editor::Project::load_prefs();
    cfg.window.width = static_cast<uint32_t>(toy::editor::get_int(prefs, "window_width", 1600));
    cfg.window.height = static_cast<uint32_t>(toy::editor::get_int(prefs, "window_height", 940));
    cfg.window.vsync = true;
    cfg.window.visible = !env_flag("HEADLESS");
    if (!cfg.window.visible) cfg.window.vsync = false;
    // The editor UI is a screen-space canvas; the game may not use one.
    cfg.render.screen_ui_enabled = true;
    cfg.output.save_on_exit = false;
    toy::editor::apply_editor_render_overrides(cfg);
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    toy::core::install_caml_codec();

    fs::path project_dir;
    fs::path scene_arg;
    fs::path package_dir;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scene" && i + 1 < argc) scene_arg = argv[++i];
        else if (a == "--new-project" && i + 1 < argc) {
            project_dir = argv[++i];
            toy::editor::Project::create(project_dir);
        } else if (a == "--package" && i + 1 < argc) package_dir = argv[++i];
        else if (a == "-h" || a == "--help") {
            std::cout << "usage: toyengine_editor [project_dir] [--scene <scene.yaml>] [--new-project <dir>] [--package <out_dir>]\n";
            return 0;
        } else if (!a.empty() && a[0] != '-') project_dir = a;
    }
    // A game project's editor (TOY_PROJECT_ROOT is its project, not this checkout) opens that
    // project; toyengine's own editor keeps the recent-projects behaviour.
    if (project_dir.empty() && std::string(TOY_PROJECT_ROOT) != std::string(ROOT_DIR)) project_dir = TOY_PROJECT_ROOT;
    if (project_dir.empty()) {
        for (const auto& r : toy::editor::Project::recent_projects()) {
            if (toy::editor::Project(r).valid()) { project_dir = r; break; }
        }
    }
    if (project_dir.empty()) project_dir = ROOT_DIR;
    toy::editor::Project project(fs::absolute(project_dir).lexically_normal());
    if (!project.valid()) {
        std::cerr << "[editor] " << project.root() << " has no .toy file or assets/ folder. Create a project with --new-project <dir>.\n";
        return 1;
    }
    // A project made by tools/toyhub starts as just a .toy + scripts: its first open lays down
    // the asset skeleton (Project::create() never overwrites anything).
    if (!project.has_assets()) {
        std::cout << "[editor] " << project.root() << ": creating assets/\n";
        project = toy::editor::Project::create(project.root());
    }

    if (!package_dir.empty()) {
        toy::editor::PackageOptions opt;
        opt.out_dir = fs::absolute(package_dir);
        opt.engine_assets = fs::path(ROOT_DIR) / "assets";
#ifdef TOY_GAME_BINARY
        if (fs::exists(TOY_GAME_BINARY)) opt.game_binary = TOY_GAME_BINARY;
#endif
        const toy::editor::PackageReport rep = toy::editor::package_project(project, opt);
        for (const auto& e : rep.errors) std::cerr << "[package] " << e << "\n";
        std::cout << "[package] " << rep.files << " files (" << rep.encoded << " encoded) -> " << opt.out_dir.string() << "\n";
        return rep.ok() ? 0 : 1;
    }

    const long max_frames = std::getenv("MAX_FRAMES") ? std::atol(std::getenv("MAX_FRAMES")) : 0;
    const char* screenshot = std::getenv("EDITOR_SCREENSHOT");

    toy::editor::EditorState state;
    bool relaunch = false;   // Build > Refresh rebuilt this binary: exec the new one below
    std::optional<toy::core::AppConfig> restart_config;
    for (;;) {
        toy::core::AppConfig base = restart_config ? *restart_config
                                                   : toy::core::AppConfig::load(project.config_path().string());
        restart_config.reset();
        toy::core::EngineOptions opts;
        opts.project_root = project.root();
        opts.load_default_scene = false;
        opts.edit_mode = true;
        opts.escape_quits = false;

        bool again = false;
        {
            toy::core::Engine engine(editor_config(base, project), opts);
            std::unique_ptr<toy::editor::EditorApp> app =
                std::make_unique<toy::editor::EditorApp>(engine, project, scene_arg, std::move(state));
            scene_arg.clear();
            state = {};
            long frames = 0;
            for (;;) {
                const bool alive = engine.tick();
                ++frames;
                if (!alive) {
                    if (app->request_close()) break;
                    engine.window().set_should_close(false);
                }
                if (app->quit_requested()) break;
                if (app->relaunch_requested()) { relaunch = true; break; }
                if (app->restart_requested()) {
                    restart_config = app->config_for_restart();
                    state = app->take_state();
                    again = true;
                    break;
                }
                if (app->switch_project_requested()) {
                    project = toy::editor::Project(fs::absolute(*app->switch_project_requested()).lexically_normal());
                    if (!project.has_assets()) project = toy::editor::Project::create(project.root());
                    again = true;
                    break;
                }
                if (max_frames > 0 && frames >= max_frames) break;
            }
            if (screenshot && !again) engine.save_screenshot(screenshot, /*low_res=*/false);
            app.reset();
        }
        if (!again) break;
    }
    if (relaunch) {
        // The Engine is gone (window, device, threads); replace this process with the freshly
        // built editor on the same project, so the new src/ code is what runs.
        const fs::path exe = toy::editor::current_executable();
        const std::string exe_s = exe.string(), proj = project.root().string();
        std::cout << "[editor] relaunching " << exe_s << " on " << proj << "\n" << std::flush;
        char* args[] = {const_cast<char*>(exe_s.c_str()), const_cast<char*>(proj.c_str()), nullptr};
        ::execv(exe_s.c_str(), args);
        std::cerr << "[editor] relaunch failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    return 0;
}
