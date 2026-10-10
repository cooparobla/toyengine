/**
 * @file runtime_paths.h
 * @brief Where a running game finds its files: the source checkout it was built from, or a
 *        relocated package (a macOS .app / a Linux folder) that carries everything with it.
 *
 * A binary built from source knows its project and the engine checkout by absolute paths
 * baked in at build time (root_directory.h: ROOT_DIR, PROJ_DIR; build_project_root()). A
 * packaged build must never consult those -- the machine it runs on has no source tree. The
 * packager (editor/build/) drops a marker file, toy_package.yaml, beside the executable (folder
 * layout) or in Contents/Resources (a .app); RuntimeLayout::detect() finds it and resolves the
 * project, asset and shader roots relative to the executable instead.
 *
 * Also here: the per-user data/log directories a shipped game writes to (never its own,
 * possibly read-only install folder, never the working directory -- a Finder-launched .app
 * runs with cwd "/"), and debug_env(), the gate that switches the engine's scripted-run env
 * hooks off in a TOY_SHIPPING build.
 */

#ifndef TOYENGINE_CORE_RUNTIME_PATHS_H
#define TOYENGINE_CORE_RUNTIME_PATHS_H

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <root_directory.h>

namespace toy::core {

/// True in a shipping build (cmake -DTOY_SHIPPING=ON): debug env hooks off, no screenshots on
/// exit, validation never requested.
#ifdef TOY_SHIPPING
inline constexpr bool k_shipping = true;
#else
inline constexpr bool k_shipping = false;
#endif

/**
 * @brief getenv() for the engine's scripted-run/debug hooks (HEADLESS, SCENE, CAPTURE_*, ...):
 *        the variable's value normally, always nullptr in a shipping build -- a player's stray
 *        environment must not reconfigure the game.
 */
const char* debug_env(const char* name);

/** @brief The running executable's absolute path (empty if it can't be found). */
std::filesystem::path executable_path();

/**
 * @brief Records the project directory this executable was built for. Called before main() by
 *        the source toyengine_add_project() compiles into the game and editor
 *        (cmake/project_root.cpp.in); the last call wins.
 */
void set_build_project_root(const char* path);

/**
 * @brief The project directory this executable was built for (set_build_project_root()). An
 *        executable that never sets it (the engine's tests, the hub) is building toyengine
 *        itself, whose project is the checkout: ROOT_DIR.
 */
const char* build_project_root();

/**
 * @struct PackageManifest
 * @brief toy_package.yaml: what the packager knew about the build. Its presence is what marks
 *        a packaged layout; the fields name the product for user directories and logs.
 */
struct PackageManifest {
    static constexpr const char* k_file_name = "toy_package.yaml";

    std::string name;           ///< Product name (the .app / executable name).
    std::string version = "0.1.0";
    std::string build = "1";
    std::string profile = "development";   ///< "development" or "shipping".
    std::string bundle_id;      ///< Reverse-DNS id; names the macOS user directories.
    std::string engine_commit;

    static std::optional<PackageManifest> load(const std::filesystem::path& path);

    void save(const std::filesystem::path& path) const;
};

/**
 * @struct RuntimeLayout
 * @brief The resolved roots for this process: Source (running from a build of the checkout)
 *        or Packaged (relocated; nothing outside the package is consulted).
 */
struct RuntimeLayout {
    enum class Mode { Source, Packaged };

    Mode mode = Mode::Source;
    std::filesystem::path exe;
    /// Packaged: the directory holding assets/ (and toy_package.yaml). Source: empty.
    std::filesystem::path resources_root;
    /// The engine checkout's assets/, the fallback layer under a project's. Empty when packaged
    /// (the packager merged everything the game needs into the package's own assets/).
    std::filesystem::path engine_assets;
    std::optional<PackageManifest> manifest;

    bool packaged() const { return mode == Mode::Packaged; }

    /**
     * @brief Finds the layout for an executable at `exe`: a toy_package.yaml beside it, or in
     *        ../Resources (exe inside X.app/Contents/MacOS), makes it Packaged. Pure apart from
     *        the filesystem checks, so tests run it on synthetic trees.
     */
    static RuntimeLayout detect(const std::filesystem::path& exe);

    /** @brief This process's layout, detected once from executable_path(). */
    static const RuntimeLayout& current();

    /**
     * @brief The project this process runs when nothing says otherwise. Packaged: the package's
     *        resources root. Source: TOY_PROJECT_DIR (not in shipping builds), else the project
     *        the binary was compiled for.
     */
    std::filesystem::path project_root() const;

    /**
     * @brief ShaderLibrary roots, first match wins. Packaged: the one merged assets/shaders.
     *        Source: the project's own shaders (when it has a distinct directory), the engine's,
     *        gfxcoopa's, then uicoopa's -- the runtime mirror of glslc's -I order.
     *
     * From source, the .spv files live in the BUILD tree this binary was compiled in
     * (TOY_SHADER_BUILD_DIR, see CMakeLists.txt), one directory per shader target -- never next
     * to the GLSL, so a game project linked to an engine checkout never writes into it. A
     * project's shaders are also looked for in its own build/ (an editor from another build
     * tree opening it).
     */
    std::vector<std::string> shader_roots(const std::filesystem::path& project_root) const;

    /** @brief Where this binary's build compiled every .spv (empty: next to the sources). */
    static std::filesystem::path compiled_shader_dir();

    /**
     * @brief The name this game's per-user directories go under: the bundle id on macOS (the
     *        Apple convention), else the product name; "toyengine/<project>" when running from
     *        source so development runs never mix with an installed copy's saves.
     */
    std::string app_id() const;
};

namespace detail {
std::filesystem::path home_dir();
std::filesystem::path xdg_dir(const char* var, const char* fallback_rel);
} // namespace detail

/**
 * @brief Per-user writable data (settings, saves): ~/Library/Application Support/<app> on
 *        macOS, $XDG_DATA_HOME/<app> (~/.local/share) on Linux. Not created here.
 */
std::filesystem::path user_data_dir(const std::string& app = RuntimeLayout::current().app_id());

/** @brief Per-user logs and crash reports: ~/Library/Logs/<app>, or $XDG_STATE_HOME/<app>/logs. */
std::filesystem::path user_log_dir(const std::string& app = RuntimeLayout::current().app_id());

/**
 * @brief Process environment a packaged game needs before the Vulkan loader starts. On macOS
 *        a package carries MoltenVK's ICD manifest in Resources/vulkan/icd.d; pointing the
 *        loader at it explicitly means a player's own installed ICDs never replace the driver
 *        the game shipped with. Idempotent; never overrides a value the user set.
 */
void prepare_runtime_environment();

} // namespace toy::core

#endif // TOYENGINE_CORE_RUNTIME_PATHS_H
