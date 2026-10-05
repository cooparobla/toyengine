/**
 * @file packager.h
 * @brief Build > Package: copies a project's assets/ to an output folder with every YAML
 *        document encoded to .caml.
 *
 * Nothing needs rewriting: every loader resolves "x.yaml" to "x.caml" when only the encoded
 * file exists (coopa::yaml::resolve_variant()), so scene references, mesh paths, LOD sidecars
 * and material references all keep working. Textures, fonts and anything else are copied
 * verbatim. The game binary decodes with TOY_CAML_KEY (or caml's default passphrase), so a
 * package built with a custom passphrase needs the same value set when it runs.
 *
 * A game project's assets/ sits over the engine checkout's (the engine's search-root fallback,
 * see Engine::asset_roots()), so the package also takes the engine's runtime directories
 * (compiled shaders, fonts, palettes, UI themes) wherever the project doesn't shadow a file --
 * never the engine's scenes or other content.
 */

#ifndef TOYEDITOR_BUILD_PACKAGER_H
#define TOYEDITOR_BUILD_PACKAGER_H

#include "../app/project.h"

#include <toyengine/core/caml_codec.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace toy::editor {

struct PackageOptions {
    std::filesystem::path out_dir;      ///< Receives out_dir/assets/...
    bool keep_yaml = false;             ///< Also copy the plain .yaml next to each .caml (debugging).
    std::string passphrase;             ///< Empty: TOY_CAML_KEY / caml's default.
    std::filesystem::path game_binary;  ///< Optional: copied to out_dir.
    /// The engine checkout's assets/ (fallback layer). Empty, or the project's own assets/,
    /// copies nothing extra.
    std::filesystem::path engine_assets;
};

/** @brief Engine assets/ subdirectories a packaged game needs at runtime. */
inline const std::vector<std::string>& engine_runtime_dirs() {
    static const std::vector<std::string> dirs = {"shaders", "fonts", "palettes", "ui"};
    return dirs;
}

struct PackageReport {
    size_t files = 0;
    size_t encoded = 0;
    uintmax_t bytes_in = 0;
    uintmax_t bytes_out = 0;
    std::vector<std::string> errors;
    bool ok() const { return errors.empty(); }
};

inline PackageReport package_project(const Project& project, const PackageOptions& opt) {
    namespace fs = std::filesystem;
    PackageReport rep;
    std::error_code ec;
    const fs::path src = project.assets();
    const fs::path dst = opt.out_dir / "assets";
    fs::create_directories(dst, ec);
    if (ec) { rep.errors.push_back("Cannot create " + dst.string() + ": " + ec.message()); return rep; }

    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { rep.errors.push_back(ec.message()); break; }
        const fs::path rel = fs::relative(it->path(), src, ec);
        const fs::path out = dst / rel;
        if (it->is_directory()) { fs::create_directories(out, ec); continue; }
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        const std::string fname = it->path().filename().string();
        if (fname.size() > 5 && fname.compare(fname.size() - 5, 5, ".tmp~") == 0) continue;
        fs::create_directories(out.parent_path(), ec);
        ++rep.files;
        rep.bytes_in += fs::file_size(it->path(), ec);
        try {
            if (ext == ".yaml" || ext == ".yml") {
                fs::path caml = out;
                caml.replace_extension(".caml");
                core::encode_caml_file(it->path(), caml, opt.passphrase);
                rep.bytes_out += fs::file_size(caml, ec);
                ++rep.encoded;
                if (opt.keep_yaml) fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
            } else {
                fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
                if (ec) rep.errors.push_back("Copy failed: " + rel.string() + ": " + ec.message());
                rep.bytes_out += fs::file_size(out, ec);
            }
        } catch (const std::exception& e) {
            rep.errors.push_back(rel.string() + ": " + e.what());
        }
    }
    std::error_code eq_ec;
    if (!opt.engine_assets.empty() && fs::is_directory(opt.engine_assets, ec) &&
        !fs::equivalent(opt.engine_assets, src, eq_ec)) {
        for (const std::string& d : engine_runtime_dirs()) {
            const fs::path base = opt.engine_assets / d;
            if (!fs::is_directory(base, ec)) continue;
            for (auto it = fs::recursive_directory_iterator(base, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) { rep.errors.push_back(ec.message()); break; }
                if (!it->is_regular_file()) continue;
                const std::string ext = it->path().extension().string();
                // Shaders ship compiled; their GLSL sources and glslc depfiles stay behind.
                if (d == "shaders" && ext != ".spv") continue;
                const fs::path rel = fs::relative(it->path(), opt.engine_assets, ec);
                fs::path out = dst / rel;
                const bool yaml = ext == ".yaml" || ext == ".yml";
                fs::path caml = out;
                caml.replace_extension(".caml");
                if (fs::exists(out, ec) || (yaml && fs::exists(caml, ec))) continue;   // the project shadows it
                fs::create_directories(out.parent_path(), ec);
                ++rep.files;
                rep.bytes_in += fs::file_size(it->path(), ec);
                try {
                    if (yaml) {
                        core::encode_caml_file(it->path(), caml, opt.passphrase);
                        rep.bytes_out += fs::file_size(caml, ec);
                        ++rep.encoded;
                    } else {
                        fs::copy_file(it->path(), out, fs::copy_options::overwrite_existing, ec);
                        if (ec) rep.errors.push_back("Copy failed: engine " + rel.string() + ": " + ec.message());
                        rep.bytes_out += fs::file_size(out, ec);
                    }
                } catch (const std::exception& e) {
                    rep.errors.push_back("engine " + rel.string() + ": " + e.what());
                }
            }
        }
    }
    if (!opt.game_binary.empty()) {
        fs::copy_file(opt.game_binary, opt.out_dir / opt.game_binary.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) rep.errors.push_back("Binary copy failed: " + ec.message());
    }
    return rep;
}

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_PACKAGER_H
