// toyengine — a pixel-art 3D engine.
//
// Renders the active scene into a small offscreen buffer, then upscales it
// to the window with nearest-neighbour filtering for a pixelated look.
//
// Build:   cbuild --vulkan     (compiles shaders and cmake builds)
// Run:     cplay               (runs ./build/toyengine on assets/config.yaml's
//                               scene.default_scene -- pixel_demo)
//          cplay physics_test  (runs a named scene under assets/scenes instead)
// Headless: HEADLESS=1 MAX_FRAMES=600 ./build/toyengine
//                               (no visible window, vsync off; prints the steady-state
//                                frame time and still saves output.save_on_exit's PNG)
// Profile:  HEADLESS=1 PROFILE=1 MAX_FRAMES=600 ./build/toyengine terrain_test
//                               (per-frame CPU/GPU timings -> output/profile.csv, or
//                                PROFILE=path.csv; per-feature averages printed on exit;
//                                graph with: python3 tools/plot_profile.py output/profile.csv)

#include <cstdlib>
#include <iostream>
#include <string>

#include <toyengine/core/config.h>
#include <toyengine/core/engine.h>

#include <root_directory.h>

namespace {

/**
 * @brief Expands a command-line scene argument into a scene.yaml path.
 *
 * Accepts either a bare scene NAME under assets/scenes (`physics_test`, which expands to
 * assets/scenes/<name>/scene.yaml -- the layout every scene in this repo uses) or an
 * explicit path to a .yaml, matching the SCENE env override's rules exactly (see
 * Engine::scene_path_from_env_()). The returned path is repo-relative; Engine resolves
 * it against ROOT_DIR.
 *
 * @param scene The argument as typed, e.g. "world_canvas_test" or "assets/scenes/x/scene.yaml".
 * @return std::string A path to a scene.yaml, relative to the repo root.
 */
std::string resolve_scene_arg(const std::string& scene) {
    if (scene.size() >= 5 && scene.compare(scene.size() - 5, 5, ".yaml") == 0) return scene;
    return "assets/scenes/" + scene + "/scene.yaml";
}

/** @brief True when env var `name` is set to anything but empty or "0". */
bool env_flag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

}  // namespace

int main(int argc, char** argv) {
    // CONFIG=<path> loads a different config file than assets/config.yaml -- for a scripted
    // run (a benchmark, an A/B of one render toggle) that should not edit the tracked file.
    std::string config_path = std::string(ROOT_DIR) + "/assets/config.yaml";
    if (const char* c = std::getenv("CONFIG"); c && *c) config_path = c;
    toy::core::AppConfig app_config = toy::core::AppConfig::load(config_path);

    // HEADLESS=1: never map the window (the same never-shown window the test suite renders
    // into) and present without vsync, so a scripted run neither takes over the desktop nor
    // is capped at the display rate. Rendering, MAX_FRAMES and output.save_on_exit's
    // screenshot all work unchanged. Pair with MAX_FRAMES -- there is no window to close.
    if (env_flag("HEADLESS")) {
        app_config.window.visible = false;
        app_config.window.vsync   = false;
    }

    // Optional scene argument: `cplay <scene>` overrides config.yaml's scene.default_scene
    // for this run only, so trying a scene never means editing a version-controlled file.
    // The SCENE env var still wins over both (Engine applies it last) -- it is the scripted-
    // capture path, and a wrapper that sets it should not be undone by a stray argument.
    if (argc > 1) app_config.scene.default_scene = resolve_scene_arg(argv[1]);

    std::cout << "==========================================================\n";
    std::cout << "                    toyengine                            \n";
    std::cout << "==========================================================\n";
    std::cout << "  Window : " << app_config.window.width << "x" << app_config.window.height
              << " (\"" << app_config.window.title << "\", VSync: "
              << (app_config.window.vsync ? "On" : "Off") << ")\n";
    std::cout << "  Scene  : " << app_config.scene.default_scene << "\n";
    std::cout << "==========================================================\n\n";

    toy::core::Engine engine(std::move(app_config));
    engine.run();

    std::cout << "\n[toyengine] Exiting cleanly after "
              << engine.frame_count() << " frames ("
              << static_cast<int>(engine.elapsed()) << "s).\n";

    return 0;
}
