/**
 * @file deps.h
 * @brief What a game binary needs at run time that a player's machine won't have: the shared
 *        libraries it links (Homebrew's GLFW / OpenSSL, ...) and, on macOS, the Vulkan loader
 *        and MoltenVK driver volk dlopen()s. Plus the small synchronous command runner the
 *        build steps share.
 *
 * The parsers work on the text otool / ldd print, so they are tested on canned output; only
 * collect_deps() and find_vulkan_runtime() touch the machine.
 */

#ifndef TOYEDITOR_BUILD_DEPS_H
#define TOYEDITOR_BUILD_DEPS_H

#include "../core/process.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace toy::editor {

// =====================================================================================
// Running commands
// =====================================================================================

/** @brief A command's exit status and combined stdout/stderr. */
struct CommandResult {
    int exit_code = -1;
    std::string output;
    bool ok() const { return exit_code == 0; }
};

/**
 * @class CommandRunner
 * @brief Runs shell commands one at a time, synchronously, streaming each output line to a
 *        sink (the build log) and honouring a shared cancel flag. Commands run with Homebrew's
 *        bin directories on PATH (tool_path_prefix()), as a Dock-launched editor lacks them.
 */
class CommandRunner {
public:
    using LineSink = std::function<void(const std::string&)>;

    explicit CommandRunner(LineSink sink = {}, const std::atomic<bool>* cancel = nullptr,
                           std::atomic<pid_t>* child = nullptr)
        : sink_(std::move(sink)), cancel_(cancel), child_(child) {}
    /** @brief Runs inside a Task job: its log, cancel flag and child-process slot. */
    explicit CommandRunner(Task::JobContext& ctx) : CommandRunner(ctx.log, ctx.cancelled, ctx.child) {}
    virtual ~CommandRunner() = default;

    bool cancelled() const { return cancel_ && cancel_->load(); }
    void log(const std::string& line) const { if (sink_) sink_(line); }

    /**
     * @brief Runs `command` via /bin/sh in its own process group (so a Task's cancel() stops it
     *        and everything it started); `echo` false keeps it out of the log (noisy queries).
     */
    virtual CommandResult run(const std::string& command, bool echo = true);

private:
    LineSink sink_;
    const std::atomic<bool>* cancel_;
    std::atomic<pid_t>* child_;
};

// =====================================================================================
// Parsing otool / ldd
// =====================================================================================

std::string trim_(const std::string& s);

/**
 * @brief The install names `otool -L <file>` lists (first line, the file's own name, skipped;
 *        for a dylib the next line is its own id, which callers filter by leaf name).
 */
std::vector<std::string> parse_otool_L(const std::string& text);

/** @brief LC_RPATH entries from `otool -l <file>`. */
std::vector<std::string> parse_otool_rpaths(const std::string& text);

/** @brief The minimum macOS (LC_BUILD_VERSION minos / LC_VERSION_MIN_MACOSX version) in `otool -l`. */
std::string parse_otool_minos(const std::string& text);

/** @brief One `ldd` line: the soname and the path it resolved to (empty if "not found"). */
struct LddEntry {
    std::string soname;
    std::string path;
};

std::vector<LddEntry> parse_ldd(const std::string& text);

/** @brief Desired host platform for is_system_dep(). */
enum class DepPlatform { MacOS, Linux };

/**
 * @brief True for libraries every player's machine already has (never bundled): macOS system
 *        libraries and frameworks; on Linux the C runtime, the graphics/windowing/audio stacks
 *        and the Vulkan loader, which must match the player's own drivers.
 */
bool is_system_dep(const std::string& path, DepPlatform platform);

// =====================================================================================
// Walking a binary's dependencies
// =====================================================================================

/** @brief A non-system library to bundle: where it is now and the name it is loaded by. */
struct BundledLib {
    std::filesystem::path source;   ///< Real file (symlinks followed).
    std::string leaf;               ///< Install-name leaf the binary asks for (libglfw.3.dylib).
};

/** @brief Resolves `@rpath/x`, `@loader_path/x`, `@executable_path/x` against an image. */
std::optional<std::filesystem::path> resolve_macho_ref_(const std::string& ref,
                                                               const std::filesystem::path& image,
                                                               const std::filesystem::path& exe,
                                                               const std::vector<std::string>& rpaths);

/**
 * @brief Breadth-first walk of `binary`'s non-system dependencies. macOS: otool -L / -l;
 *        Linux: ldd (already transitive). Unresolvable references are reported in `missing`.
 */
std::vector<BundledLib> collect_deps(const std::filesystem::path& binary, CommandRunner& run,
                                            std::vector<std::string>* missing = nullptr);

/** @brief macOS: the Vulkan loader, MoltenVK and MoltenVK's ICD manifest to bundle. */
struct VulkanRuntime {
    std::filesystem::path loader;      ///< libvulkan.1.dylib
    std::filesystem::path moltenvk;    ///< libMoltenVK.dylib
    std::filesystem::path icd_json;    ///< MoltenVK_icd.json (its api_version is kept)
    bool complete() const { return !loader.empty() && !moltenvk.empty(); }
};

/** @brief Searches $VULKAN_SDK, Homebrew and /usr/local for the macOS Vulkan runtime. */
VulkanRuntime find_vulkan_runtime();

/**
 * @brief MoltenVK_icd.json rewritten to point at the bundled driver: `library_path` replaced,
 *        everything else (api_version, file_format_version) kept as installed.
 */
std::string rewrite_icd_json(const std::string& json, const std::string& library_path);

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_DEPS_H
