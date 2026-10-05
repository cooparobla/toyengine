/**
 * @file runtime_paths.h
 * @brief Where a running game finds its files: the source checkout it was built from, or a
 *        relocated package (a macOS .app / a Linux folder) that carries everything with it.
 *
 * A binary built from source knows its project and the engine checkout by absolute paths
 * baked in at configure time (root_directory.h: ROOT_DIR, PROJ_DIR, TOY_PROJECT_ROOT). A
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

#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

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
inline const char* debug_env(const char* name) {
    if constexpr (k_shipping) { (void)name; return nullptr; }
    return std::getenv(name);
}

/** @brief The running executable's absolute path (empty if it can't be found). */
inline std::filesystem::path executable_path() {
    std::error_code ec;
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return std::filesystem::weakly_canonical(buf, ec);
    return {};
#else
    return std::filesystem::read_symlink("/proc/self/exe", ec);
#endif
}

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

    static std::optional<PackageManifest> load(const std::filesystem::path& path) {
        std::optional<fkyaml::node> doc;
        try { doc = coopa::yaml::try_load_document(path); } catch (...) { return std::nullopt; }
        if (!doc || !doc->is_mapping()) return std::nullopt;
        PackageManifest m;
        auto str = [&](const char* key, std::string& out) {
            if (doc->contains(key) && (*doc)[key].is_string()) out = (*doc)[key].get_value<std::string>();
        };
        str("name", m.name);
        str("version", m.version);
        str("build", m.build);
        str("profile", m.profile);
        str("bundle_id", m.bundle_id);
        str("engine_commit", m.engine_commit);
        return m;
    }

    void save(const std::filesystem::path& path) const {
        fkyaml::node n = fkyaml::node::mapping();
        n["name"] = fkyaml::node(name);
        n["version"] = fkyaml::node(version);
        n["build"] = fkyaml::node(build);
        n["profile"] = fkyaml::node(profile);
        n["bundle_id"] = fkyaml::node(bundle_id);
        n["engine_commit"] = fkyaml::node(engine_commit);
        coopa::yaml::save_document(path, n);
    }
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
    static RuntimeLayout detect(const std::filesystem::path& exe) {
        RuntimeLayout l;
        l.exe = exe;
        std::error_code ec;
        if (!exe.empty()) {
            const std::filesystem::path dir = exe.parent_path();
            const std::filesystem::path candidates[] = {dir, dir.parent_path() / "Resources"};
            for (const auto& c : candidates) {
                if (std::filesystem::is_regular_file(c / PackageManifest::k_file_name, ec)) {
                    l.mode = Mode::Packaged;
                    l.resources_root = c;
                    l.manifest = PackageManifest::load(c / PackageManifest::k_file_name);
                    return l;
                }
            }
        }
        l.engine_assets = std::filesystem::path(ROOT_DIR) / "assets";
        return l;
    }

    /** @brief This process's layout, detected once from executable_path(). */
    static const RuntimeLayout& current() {
        static const RuntimeLayout layout = detect(executable_path());
        return layout;
    }

    /**
     * @brief The project this process runs when nothing says otherwise. Packaged: the package's
     *        resources root. Source: TOY_PROJECT_DIR (not in shipping builds), else the project
     *        the binary was compiled for.
     */
    std::filesystem::path project_root() const {
        if (packaged()) return resources_root;
        if (const char* p = debug_env("TOY_PROJECT_DIR"); p && *p) return std::filesystem::path(p);
        return std::filesystem::path(TOY_PROJECT_ROOT);
    }

    /**
     * @brief ShaderLibrary roots, first match wins. Packaged: the one merged assets/shaders.
     *        Source: the project's own shaders (when it has a distinct directory), the engine's,
     *        gfxcoopa's base library, then uicoopa's -- the runtime mirror of glslc's -I order.
     */
    std::vector<std::string> shader_roots(const std::filesystem::path& project_root) const {
        const std::filesystem::path project_shaders = project_root / "assets" / "shaders";
        if (packaged()) return {project_shaders.string()};
        const std::filesystem::path engine_shaders = engine_assets / "shaders";
        std::vector<std::string> roots;
        std::error_code ec;
        if (std::filesystem::is_directory(project_shaders, ec) &&
            !std::filesystem::equivalent(project_shaders, engine_shaders, ec)) {
            roots.push_back(project_shaders.string());
        }
        roots.push_back(engine_shaders.string());
        roots.push_back(std::string(PROJ_DIR) + "/gfxcoopa/assets/shaders");
        roots.push_back(std::string(PROJ_DIR) + "/uicoopa/assets/shaders");
        return roots;
    }

    /**
     * @brief The name this game's per-user directories go under: the bundle id on macOS (the
     *        Apple convention), else the product name; "toyengine/<project>" when running from
     *        source so development runs never mix with an installed copy's saves.
     */
    std::string app_id() const {
        if (manifest) {
#if defined(__APPLE__)
            if (!manifest->bundle_id.empty()) return manifest->bundle_id;
#endif
            if (!manifest->name.empty()) return manifest->name;
        }
        std::string project = project_root().filename().string();
        if (project.empty()) project = "game";
        return "toyengine-dev/" + project;
    }
};

namespace detail {
inline std::filesystem::path home_dir() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home && *home ? home : "/tmp");
}
inline std::filesystem::path xdg_dir(const char* var, const char* fallback_rel) {
    if (const char* v = std::getenv(var); v && *v) return v;
    return home_dir() / fallback_rel;
}
} // namespace detail

/**
 * @brief Per-user writable data (settings, saves): ~/Library/Application Support/<app> on
 *        macOS, $XDG_DATA_HOME/<app> (~/.local/share) on Linux. Not created here.
 */
inline std::filesystem::path user_data_dir(const std::string& app = RuntimeLayout::current().app_id()) {
#if defined(__APPLE__)
    return detail::home_dir() / "Library" / "Application Support" / app;
#else
    return detail::xdg_dir("XDG_DATA_HOME", ".local/share") / app;
#endif
}

/** @brief Per-user logs and crash reports: ~/Library/Logs/<app>, or $XDG_STATE_HOME/<app>/logs. */
inline std::filesystem::path user_log_dir(const std::string& app = RuntimeLayout::current().app_id()) {
#if defined(__APPLE__)
    return detail::home_dir() / "Library" / "Logs" / app;
#else
    return detail::xdg_dir("XDG_STATE_HOME", ".local/state") / app / "logs";
#endif
}

/**
 * @brief Process environment a packaged game needs before the Vulkan loader starts. On macOS
 *        a package carries MoltenVK's ICD manifest in Resources/vulkan/icd.d; pointing the
 *        loader at it explicitly means a player's own installed ICDs never replace the driver
 *        the game shipped with. Idempotent; never overrides a value the user set.
 */
inline void prepare_runtime_environment() {
    static bool done = false;
    if (done) return;
    done = true;
    const RuntimeLayout& layout = RuntimeLayout::current();
    if (!layout.packaged()) return;
#if defined(__APPLE__)
    const std::filesystem::path icd = layout.resources_root / "vulkan" / "icd.d" / "MoltenVK_icd.json";
    std::error_code ec;
    if (std::filesystem::is_regular_file(icd, ec)) {
        ::setenv("VK_DRIVER_FILES", icd.c_str(), 0);
        ::setenv("VK_ICD_FILENAMES", icd.c_str(), 0);
    }
#endif
}

} // namespace toy::core

#endif // TOYENGINE_CORE_RUNTIME_PATHS_H
