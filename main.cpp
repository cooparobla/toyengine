// toyengine — a pixel-art 3D engine.
//
// Renders the active scene into a small offscreen buffer, then upscales it
// to the window with nearest-neighbour filtering for a pixelated look.
//
// Build:   cbuild --vulkan     (compiles shaders and cmake builds)
// Run:     cplay               (runs ./build/toyengine)

// volk and VMA implementations are compiled once here, in this single
// translation unit -- see CMakeLists.txt's add_definitions() comment.
#include <volk/volk.h>

#define VMA_STATIC_VULKAN_FUNCTIONS  0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vma/vk_mem_alloc.h>

#include <iostream>
#include <string>

// stb single-header libraries guard their declarations but not their
// implementation block against multiple #includes in one translation unit
// (see CMakeLists.txt's comment) -- more than one toyengine header pulls in
// stb_image.h/stb_image_write.h, so each implementation is triggered exactly
// once here, before anything else in this TU includes them undefined.
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#undef STB_IMAGE_IMPLEMENTATION

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#undef STB_IMAGE_WRITE_IMPLEMENTATION

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
              << engine.time().frame_count() << " frames ("
              << static_cast<int>(engine.time().elapsed()) << "s).\n";

    return 0;
}
