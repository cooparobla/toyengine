/**
 * @file packager.h
 * @brief Stages a project's runtime assets into an output folder: Build > Package, and the asset
 *        step of Build (build_pipeline.h).
 *
 * Nothing needs rewriting: every loader resolves "x.yaml" to "x.caml" when only the encoded
 * file exists (coopa::yaml::resolve_variant()), so scene references, mesh paths, LOD sidecars
 * and material references all keep working whether or not YAML is encoded. Textures, fonts,
 * audio and anything else are copied verbatim. The game binary decodes with its baked key,
 * TOY_CAML_KEY, or caml's default passphrase, so a package built with a custom passphrase
 * needs the same value at run time.
 *
 * The output is self-contained, in layers, first file wins (a higher layer shadows a lower):
 *   1. the project's own assets/
 *   2. the engine checkout's runtime directories (compiled shaders, fonts, palettes, UI themes)
 *      -- never the engine's scenes or other content
 *   3. library layers: gfxcoopa's and uicoopa's compiled shaders merged into assets/shaders
 *      (the same first-match order the runtime's ShaderLibrary uses from source, so the one
 *      merged directory resolves every name to the same file), and uicoopa's default UI sounds
 * A packaged build reads only this folder (toyengine/core/runtime_paths.h).
 */

#ifndef TOYEDITOR_BUILD_PACKAGER_H
#define TOYEDITOR_BUILD_PACKAGER_H

#include "../app/project.h"

#include <toyengine/core/caml_codec.h>

#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <root_directory.h>

namespace toy::editor {

/** @brief A directory merged into the package under `dest` (relative to assets/). */
struct PackageLayer {
    std::filesystem::path src;
    std::string dest;            ///< e.g. "shaders"
    bool compiled_shaders = false;   ///< Only .spv files (GLSL sources and depfiles stay behind).
};

struct PackageOptions {
    std::filesystem::path out_dir;      ///< Receives out_dir/assets/...
    bool encode_yaml = true;            ///< Encode YAML documents to .caml (false: copy as plain YAML).
    bool keep_yaml = false;             ///< With encode_yaml: also copy the plain .yaml (debugging).
    bool shipping = false;              ///< Rewrites config.yaml for a player (no save_on_exit screenshot).
    std::string passphrase;             ///< Empty: TOY_CAML_KEY / caml's default.
    std::filesystem::path game_binary;  ///< Optional: copied to out_dir.
    /// The engine checkout's assets/ (fallback layer). Empty, or the project's own assets/,
    /// copies nothing extra.
    std::filesystem::path engine_assets;
    /// Library layers below the engine's (default_library_layers()).
    std::vector<PackageLayer> library_layers;
};

/** @brief Engine assets/ subdirectories a packaged game needs at runtime. */
inline const std::vector<std::string>& engine_runtime_dirs() {
    static const std::vector<std::string> dirs = {"shaders", "fonts", "palettes", "ui", "sounds"};
    return dirs;
}

/**
 * @brief The coopa libraries' runtime files from this checkout: gfxcoopa's and uicoopa's
 *        compiled shaders (in the runtime's search order) and uicoopa's default UI sounds.
 */
inline std::vector<PackageLayer> default_library_layers(const std::filesystem::path& libs_dir = PROJ_DIR) {
    return {
        {libs_dir / "gfxcoopa" / "assets" / "shaders", "shaders", true},
        {libs_dir / "uicoopa" / "assets" / "shaders", "shaders", true},
        {libs_dir / "uicoopa" / "assets" / "sounds", "sounds", false},
    };
}

struct PackageReport {
    size_t files = 0;
    size_t encoded = 0;
    uintmax_t bytes_in = 0;
    uintmax_t bytes_out = 0;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    bool ok() const { return errors.empty(); }
};

namespace detail {

inline bool is_yaml_ext_(const std::string& ext) { return ext == ".yaml" || ext == ".yml"; }

/** @brief config.yaml as a player gets it: no screenshot written on quit. */
inline std::string shipping_config_text_(const std::filesystem::path& src) {
    fkyaml::node doc = coopa::yaml::load_document(src);
    if (doc.is_mapping()) {
        if (!doc.contains("output") || !doc["output"].is_mapping()) doc["output"] = fkyaml::node::mapping();
        doc["output"]["save_on_exit"] = fkyaml::node(false);
    }
    return coopa::yaml::emit(doc);
}

/**
 * @brief Writes one file into the package: YAML documents encoded (or copied plain), the root
 *        config.yaml rewritten for shipping, everything else copied byte for byte.
 */
inline void stage_file_(const std::filesystem::path& from, const std::filesystem::path& out,
                        bool is_root_config, const PackageOptions& opt, PackageReport& rep) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string ext = from.extension().string();
    fs::create_directories(out.parent_path(), ec);
    ++rep.files;
    rep.bytes_in += fs::file_size(from, ec);
    if (is_yaml_ext_(ext)) {
        const std::string text = is_root_config && opt.shipping ? shipping_config_text_(from)
                                                                : coopa::yaml::read_text(from);
        if (opt.encode_yaml) {
            fs::path caml = out;
            caml.replace_extension(".caml");
            const std::vector<uint8_t> bytes = core::encode_caml_text(text, opt.passphrase);
            std::ofstream f(caml, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!f) throw std::runtime_error("cannot write " + caml.string());
            rep.bytes_out += bytes.size();
            ++rep.encoded;
            if (opt.keep_yaml) std::ofstream(out, std::ios::binary | std::ios::trunc) << text;
        } else {
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            f << text;
            if (!f) throw std::runtime_error("cannot write " + out.string());
            rep.bytes_out += text.size();
        }
        return;
    }
    fs::copy_file(from, out, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("copy failed: " + ec.message());
    rep.bytes_out += fs::file_size(out, ec);
}

/** @brief True if `out` (or its .caml twin) is already staged by a higher layer. */
inline bool shadowed_(const std::filesystem::path& out) {
    std::error_code ec;
    if (std::filesystem::exists(out, ec)) return true;
    if (is_yaml_ext_(out.extension().string())) {
        std::filesystem::path caml = out;
        caml.replace_extension(".caml");
        return std::filesystem::exists(caml, ec);
    }
    return false;
}

/**
 * @brief Copies `src` (recursively) into `dst`, skipping files a higher layer already staged.
 *        `filter` rejects files by path; collisions of compiled shaders are reported as warnings.
 */
inline void copy_layer_(const std::filesystem::path& src, const std::filesystem::path& dst,
                        const std::string& label, bool compiled_shaders, bool warn_collisions,
                        const PackageOptions& opt, PackageReport& rep) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(src, ec)) return;
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { rep.errors.push_back(ec.message()); break; }
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        const std::string fname = it->path().filename().string();
        if (fname.size() > 5 && fname.compare(fname.size() - 5, 5, ".tmp~") == 0) continue;
        if (fname == ".DS_Store") continue;
        // Shaders ship compiled; their GLSL sources and glslc depfiles stay behind.
        if (compiled_shaders && ext != ".spv") continue;
        const fs::path rel = fs::relative(it->path(), src, ec);
        const fs::path out = dst / rel;
        if (shadowed_(out)) {
            if (warn_collisions) rep.warnings.push_back(label + " " + rel.string() + " is shadowed by a higher layer");
            continue;
        }
        try {
            stage_file_(it->path(), out, false, opt, rep);
        } catch (const std::exception& e) {
            rep.errors.push_back(label + " " + rel.string() + ": " + e.what());
        }
    }
}

} // namespace detail

inline PackageReport package_project(const Project& project, const PackageOptions& opt) {
    namespace fs = std::filesystem;
    PackageReport rep;
    std::error_code ec;
    const fs::path src = project.assets();
    const fs::path dst = opt.out_dir / "assets";
    fs::create_directories(dst, ec);
    if (ec) { rep.errors.push_back("Cannot create " + dst.string() + ": " + ec.message()); return rep; }

    // 1. The project's own assets/: everything (shaders compiled only).
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { rep.errors.push_back(ec.message()); break; }
        const fs::path rel = fs::relative(it->path(), src, ec);
        const fs::path out = dst / rel;
        if (it->is_directory()) { fs::create_directories(out, ec); continue; }
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        const std::string fname = it->path().filename().string();
        if (fname.size() > 5 && fname.compare(fname.size() - 5, 5, ".tmp~") == 0) continue;
        if (fname == ".DS_Store") continue;
        const bool in_shaders = rel.begin() != rel.end() && *rel.begin() == "shaders";
        if (in_shaders && ext != ".spv") continue;
        try {
            detail::stage_file_(it->path(), out, rel == fs::path("config.yaml"), opt, rep);
        } catch (const std::exception& e) {
            rep.errors.push_back(rel.string() + ": " + e.what());
        }
    }

    // 2. The engine checkout's runtime directories, where the project doesn't shadow them.
    std::error_code eq_ec;
    if (!opt.engine_assets.empty() && fs::is_directory(opt.engine_assets, ec) &&
        !fs::equivalent(opt.engine_assets, src, eq_ec)) {
        for (const std::string& d : engine_runtime_dirs()) {
            detail::copy_layer_(opt.engine_assets / d, dst / d, "engine", d == "shaders", false, opt, rep);
        }
    }

    // 3. Library layers (gfxcoopa / uicoopa shaders, uicoopa's default sounds).
    // The first shader layer (gfxcoopa's) sits directly under the engine's, the runtime's -I
    // order, and is copied without clash warnings; a LATER library's shader losing to an
    // earlier layer is a real name clash and warns.
    bool first_shader_layer = true;
    for (const PackageLayer& layer : opt.library_layers) {
        const bool warn = layer.compiled_shaders && !first_shader_layer;
        if (layer.compiled_shaders) first_shader_layer = false;
        detail::copy_layer_(layer.src, dst / layer.dest, layer.src.parent_path().parent_path().filename().string(),
                            layer.compiled_shaders, warn, opt, rep);
    }

    if (!opt.game_binary.empty()) {
        fs::copy_file(opt.game_binary, opt.out_dir / opt.game_binary.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) rep.errors.push_back("Binary copy failed: " + ec.message());
    }
    return rep;
}

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_PACKAGER_H
