/**
 * @file export_icon.cpp
 * @brief Writes the toyengine logo (toyengine/core/branding.h) as app-icon files.
 *
 *   toyengine_icon [out_dir]      (default: <repo>/editor/branding)
 *
 * Produces toyengine_icon_<N>.png for N = 16 ... 1024, and on macOS a toyengine.icns (via
 * iconutil) for an app bundle. The running engine doesn't need these -- it rasterizes the
 * same shapes at startup for its window / Dock icon -- they are for packaging and docs.
 */

#include <toyengine/core/branding.h>

#include <stb/stb_image_write.h>

#include <root_directory.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static bool write_png(const fs::path& path, int size) {
    const std::vector<uint8_t> px = toy::core::rasterize_logo(size);
    return stbi_write_png(path.string().c_str(), size, size, 4, px.data(), size * 4) != 0;
}

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::path(ROOT_DIR) / "editor" / "branding";
    fs::create_directories(out);
    for (int size : {16, 32, 48, 64, 128, 256, 512, 1024}) {
        const fs::path p = out / ("toyengine_icon_" + std::to_string(size) + ".png");
        if (!write_png(p, size)) { std::cerr << "failed to write " << p << "\n"; return 1; }
        std::cout << "wrote " << p.string() << "\n";
    }
#ifdef __APPLE__
    // An .iconset folder (Apple's names, 1x and 2x) -> iconutil -> .icns.
    const fs::path set = out / "toyengine.iconset";
    fs::create_directories(set);
    for (int base : {16, 32, 128, 256, 512}) {
        write_png(set / ("icon_" + std::to_string(base) + "x" + std::to_string(base) + ".png"), base);
        write_png(set / ("icon_" + std::to_string(base) + "x" + std::to_string(base) + "@2x.png"), base * 2);
    }
    const std::string cmd = "iconutil -c icns \"" + set.string() + "\" -o \"" + (out / "toyengine.icns").string() + "\"";
    if (std::system(cmd.c_str()) == 0) {
        std::cout << "wrote " << (out / "toyengine.icns").string() << "\n";
        fs::remove_all(set);
    } else {
        std::cerr << "iconutil failed; the PNGs are still written (iconset left at " << set.string() << ")\n";
    }
#endif
    return 0;
}
