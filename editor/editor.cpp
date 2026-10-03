// toyengine editor -- scenes, meshes, materials and render settings for toyengine projects.
//
// Usage:   toyengine_editor [project_dir] [--scene <scene.yaml>] [--new-project <dir>]
//          (no project: the most recent one, else this repository's own assets/)
// Headless smoke run (no visible window):
//          HEADLESS=1 MAX_FRAMES=60 EDITOR_SCREENSHOT=out.png ./build/toyengine_editor
//
// The editor embeds the real toy::core::Engine, so "Full Render" in its viewport is the
// game's own PixelRenderPipeline. A renderer restart (startup-only render settings) or a
// project switch tears the Engine down and builds a new one; open documents survive a
// restart via EditorState.

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#include <toyengine/core/caml_codec.h>
#include <toyengine/core/config.h>
#include <toyengine/core/engine.h>

#include "app/editor_app.h"

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
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    toy::core::install_caml_codec();

    fs::path project_dir;
    fs::path scene_arg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scene" && i + 1 < argc) scene_arg = argv[++i];
        else if (a == "--new-project" && i + 1 < argc) {
            project_dir = argv[++i];
            toy::editor::Project::create(project_dir);
        } else if (a == "-h" || a == "--help") {
            std::cout << "usage: toyengine_editor [project_dir] [--scene <scene.yaml>] [--new-project <dir>]\n";
            return 0;
        } else if (!a.empty() && a[0] != '-') project_dir = a;
    }
    if (project_dir.empty()) {
        for (const auto& r : toy::editor::Project::recent_projects()) {
            if (toy::editor::Project(r).valid()) { project_dir = r; break; }
        }
    }
    if (project_dir.empty()) project_dir = ROOT_DIR;
    toy::editor::Project project(fs::absolute(project_dir).lexically_normal());
    if (!project.valid()) {
        std::cerr << "[editor] " << project.root() << " has no assets/ folder. Create a project with --new-project <dir>.\n";
        return 1;
    }

    const long max_frames = std::getenv("MAX_FRAMES") ? std::atol(std::getenv("MAX_FRAMES")) : 0;
    const char* screenshot = std::getenv("EDITOR_SCREENSHOT");

    toy::editor::EditorState state;
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
                if (app->restart_requested()) {
                    restart_config = app->config_for_restart();
                    state = app->take_state();
                    again = true;
                    break;
                }
                if (app->switch_project_requested()) {
                    project = toy::editor::Project(fs::absolute(*app->switch_project_requested()).lexically_normal());
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
    return 0;
}
