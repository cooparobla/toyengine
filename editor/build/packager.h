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
 *   1. the project's own assets/ (minus shaders/: its GLSL never ships, its .spv come in 3.)
 *   2. the engine checkout's runtime directories (fonts, palettes, UI themes, sounds)
 *      -- never the engine's scenes or other content
 *   3. library layers: the compiled shaders -- the project's, the engine's, gfxcoopa's and
 *      uicoopa's, from the BUILD tree they were compiled into (RuntimeLayout::shader_roots(),
 *      the same first-match order the runtime's ShaderLibrary uses from source, so the one
 *      merged assets/shaders resolves every name to the same file) -- and uicoopa's default
 *      UI sounds
 * A packaged build reads only this folder (toyengine/core/runtime_paths.h).
 */

#ifndef TOYEDITOR_BUILD_PACKAGER_H
#define TOYEDITOR_BUILD_PACKAGER_H

#include "../app/project.h"

#include <toyengine/core/runtime_paths.h>

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
    bool warn_shadowed = false;      ///< A file a higher layer already staged is a real clash: warn.
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

/** @brief Engine assets/ subdirectories a packaged game needs at runtime (shaders come compiled, as layers). */
const std::vector<std::string>& engine_runtime_dirs();

/**
 * @brief The compiled shaders `project` runs with, from the build tree (in the runtime's search
 *        order: the project's own, the engine's, gfxcoopa's, uicoopa's), then uicoopa's default
 *        UI sounds. A uicoopa shader losing to an earlier layer is a real name clash and warns.
 */
std::vector<PackageLayer> default_library_layers(const std::filesystem::path& project_root,
                                                        const std::filesystem::path& libs_dir = PROJ_DIR);

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
std::string shipping_config_text_(const std::filesystem::path& src);

/**
 * @brief Writes one file into the package: YAML documents encoded (or copied plain), the root
 *        config.yaml rewritten for shipping, everything else copied byte for byte.
 */
void stage_file_(const std::filesystem::path& from, const std::filesystem::path& out,
                        bool is_root_config, const PackageOptions& opt, PackageReport& rep);

/** @brief True if `out` (or its .caml twin) is already staged by a higher layer. */
bool shadowed_(const std::filesystem::path& out);

/**
 * @brief Copies `src` (recursively) into `dst`, skipping files a higher layer already staged.
 *        `filter` rejects files by path; collisions of compiled shaders are reported as warnings.
 */
void copy_layer_(const std::filesystem::path& src, const std::filesystem::path& dst,
                        const std::string& label, bool compiled_shaders, bool warn_collisions,
                        const PackageOptions& opt, PackageReport& rep);

} // namespace detail

PackageReport package_project(const Project& project, const PackageOptions& opt);

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_PACKAGER_H
