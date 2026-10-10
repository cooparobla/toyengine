/**
 * @file build_pipeline.h
 * @brief Build: compile the game, stage its assets, and bundle a relocatable, signed package
 *        for the host platform -- shared by the editor's Build menu and `--build` on the CLI.
 *
 *   Development   the editor's own build tree, incremental; plain YAML; ad-hoc signed.
 *                 Fast to iterate on; debug env hooks and validation still available.
 *   Shipping      a separate Release tree (<project>/build-ship, -DTOY_SHIPPING=ON); YAML
 *                 encoded to .caml; Developer ID signed with the hardened runtime, notarized
 *                 and stapled on macOS (ad-hoc fallback when no identity exists); archived.
 *
 * Both produce the same layout -- <output>/<profile>/<Product>.app on macOS, a <Product>/
 * folder on Linux -- that runs from anywhere without the source tree, Homebrew or the Vulkan
 * SDK (toyengine/core/runtime_paths.h).
 */

#ifndef TOYEDITOR_BUILD_BUILD_PIPELINE_H
#define TOYEDITOR_BUILD_BUILD_PIPELINE_H

#include "build_settings.h"
#include "bundle_linux.h"
#include "bundle_macos.h"
#include "deps.h"
#include "packager.h"

#include <toyengine/core/runtime_paths.h>

#include <filesystem>
#include <optional>
#include <string>

#include <root_directory.h>

namespace toy::editor {

/** @brief How this editor binary was built: where Build finds/compiles the game. */
struct BuildEnvironment {
    std::filesystem::path build_dir;     ///< The editor's build tree (Development compiles here).
    std::string game_target;             ///< cmake target of the game executable.
    std::filesystem::path source_dir;    ///< Top-level CMakeLists.txt directory (Shipping configures from it).
    std::filesystem::path game_binary;   ///< The Development game executable.

    /**
     * @brief Records this editor executable's environment. Called before main() by the source
     *        toyengine_add_project() generates for the editor (cmake/editor_build_env.cpp.in).
     */
    static void set_compiled(BuildEnvironment env) { compiled_slot_() = std::move(env); }

    /** @brief Whether set_compiled() ran: a toyengine_add_project() editor, not a test or tool. */
    static bool compiled() { return compiled_slot_().has_value(); }

    /**
     * @brief This editor's own environment: the registered one, else (tests, tools) the
     *        engine checkout's default build/ tree and its toyengine game.
     */
    static BuildEnvironment current();

    /**
     * @brief Whether this editor's game binary carries `project`'s code: a game project's
     *        editor only builds its own project; toyengine's own game is the generic player any
     *        assets-only project runs on.
     */
    bool builds(const Project& project) const;

private:
    static std::optional<BuildEnvironment>& compiled_slot_();
};

struct BuildRequest {
    BuildProfile profile = BuildProfile::Development;
    std::filesystem::path out_dir;       ///< Empty: settings.output_path()/<profile>.
    bool skip_compile = false;           ///< Use the existing binary as is (tests, re-bundling).
    std::filesystem::path binary_override;   ///< Bundle this executable instead (tests).
};

struct BuildResult {
    bool ok = false;
    std::filesystem::path artifact;      ///< The .app / folder.
    std::filesystem::path archive;
    std::filesystem::path executable;    ///< Inside the artifact (what Build and Run launches).
    std::string signed_with;
    bool notarized = false;
    std::vector<std::string> warnings;
    std::string error;
};

/** @brief The executable inside an artifact, and the command that launches it detached. */
std::string artifact_launch_command(const std::filesystem::path& artifact, const std::string& exe_name);

/** @brief Runs a whole build synchronously, logging through `run`. */
BuildResult run_build(const Project& project, const BuildSettings& settings, const BuildRequest& req,
                             const BuildEnvironment& env, CommandRunner& run);

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_BUILD_PIPELINE_H
