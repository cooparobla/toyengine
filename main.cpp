// toyengine — a pixel-art 3D engine.
//
// Renders the active scene into a small offscreen buffer, then upscales it
// to the window with nearest-neighbour filtering for a pixelated look.
//
// Build:   cbuild --vulkan     (compiles shaders and cmake builds)
// Run:     cplay               (runs ./build/toyengine on assets/config.yaml's
//                               scene.default_scene -- pixel_demo)
//          cplay physics_test  (runs a named scene under assets/scenes instead)

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

}  // namespace

int main(int argc, char** argv) {
    std::string config_path = std::string(ROOT_DIR) + "/assets/config.yaml";
    toy::core::AppConfig app_config = toy::core::AppConfig::load(config_path);

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
