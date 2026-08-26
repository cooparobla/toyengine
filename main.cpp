// toyengine — a pixel-art 3D engine.
//
// Renders the active scene into a small offscreen buffer, then upscales it
// to the window with nearest-neighbour filtering for a pixelated look.
//
// Build:   cbuild --vulkan     (compiles shaders and cmake builds)
// Run:     cplay               (runs ./build/toyengine)

#include <iostream>
#include <string>

#include <toyengine/core/config.h>
#include <toyengine/core/engine.h>

#include <root_directory.h>

int main() {
    std::string config_path = std::string(ROOT_DIR) + "/assets/config.yaml";
    toy::core::AppConfig app_config = toy::core::AppConfig::load(config_path);

    std::cout << "==========================================================\n";
    std::cout << "                    toyengine                            \n";
    std::cout << "==========================================================\n";
    std::cout << "  Window : " << app_config.window.width << "x" << app_config.window.height
              << " (\"" << app_config.window.title << "\", VSync: "
              << (app_config.window.vsync ? "On" : "Off") << ")\n";
    std::cout << "==========================================================\n\n";

    toy::core::Engine engine(std::move(app_config));
    engine.run();

    std::cout << "\n[toyengine] Exiting cleanly after "
              << engine.frame_count() << " frames ("
              << static_cast<int>(engine.elapsed()) << "s).\n";

    return 0;
}
