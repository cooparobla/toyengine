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
    virtual CommandResult run(const std::string& command, bool echo = true) {
        CommandResult r;
        if (cancelled()) { r.exit_code = 130; return r; }
        if (echo) log("$ " + command);
        const std::string full = tool_path_prefix() + command;
        int fds[2];
        if (::pipe(fds) != 0) { r.output = "pipe failed"; return r; }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&fa, fds[0]);
        posix_spawn_file_actions_addclose(&fa, fds[1]);
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
        std::string cmd = full;
        char* argv[] = {const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), cmd.data(), nullptr};
        pid_t pid = 0;
        const int rc = ::posix_spawn(&pid, "/bin/sh", &fa, &attr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&attr);
        ::close(fds[1]);
        if (rc != 0) { ::close(fds[0]); r.output = "spawn failed"; return r; }
        if (child_) *child_ = pid;
        std::string pending;
        char buf[4096];
        ssize_t n;
        while ((n = ::read(fds[0], buf, sizeof(buf))) > 0) {
            r.output.append(buf, static_cast<size_t>(n));
            pending.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                if (echo) log(pending.substr(0, nl));
                pending.erase(0, nl + 1);
            }
        }
        if (!pending.empty() && echo) log(pending);
        ::close(fds[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        if (child_) *child_ = 0;
        r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        return r;
    }

private:
    LineSink sink_;
    const std::atomic<bool>* cancel_;
    std::atomic<pid_t>* child_;
};

// =====================================================================================
// Parsing otool / ldd
// =====================================================================================

inline std::string trim_(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/**
 * @brief The install names `otool -L <file>` lists (first line, the file's own name, skipped;
 *        for a dylib the next line is its own id, which callers filter by leaf name).
 */
inline std::vector<std::string> parse_otool_L(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) { first = false; if (!line.empty() && line[0] != '\t' && line[0] != ' ') continue; }
        const std::string t = trim_(line);
        if (t.empty()) continue;
        const size_t paren = t.find(" (");
        out.push_back(paren == std::string::npos ? t : t.substr(0, paren));
    }
    return out;
}

/** @brief LC_RPATH entries from `otool -l <file>`. */
inline std::vector<std::string> parse_otool_rpaths(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    bool in_rpath = false;
    while (std::getline(in, line)) {
        const std::string t = trim_(line);
        if (t.rfind("cmd ", 0) == 0) { in_rpath = t == "cmd LC_RPATH"; continue; }
        if (in_rpath && t.rfind("path ", 0) == 0) {
            std::string p = t.substr(5);
            const size_t paren = p.find(" (offset");
            if (paren != std::string::npos) p = p.substr(0, paren);
            out.push_back(trim_(p));
            in_rpath = false;
        }
    }
    return out;
}

/** @brief The minimum macOS (LC_BUILD_VERSION minos / LC_VERSION_MIN_MACOSX version) in `otool -l`. */
inline std::string parse_otool_minos(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    bool in_cmd = false;
    while (std::getline(in, line)) {
        const std::string t = trim_(line);
        if (t.rfind("cmd ", 0) == 0) { in_cmd = t == "cmd LC_BUILD_VERSION" || t == "cmd LC_VERSION_MIN_MACOSX"; continue; }
        if (in_cmd && (t.rfind("minos ", 0) == 0 || t.rfind("version ", 0) == 0)) return trim_(t.substr(t.find(' ') + 1));
    }
    return {};
}

/** @brief One `ldd` line: the soname and the path it resolved to (empty if "not found"). */
struct LddEntry {
    std::string soname;
    std::string path;
};

inline std::vector<LddEntry> parse_ldd(const std::string& text) {
    std::vector<LddEntry> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const std::string t = trim_(line);
        if (t.empty()) continue;
        const size_t arrow = t.find(" => ");
        LddEntry e;
        if (arrow == std::string::npos) {
            // "/lib64/ld-linux-x86-64.so.2 (0x...)" or "linux-vdso.so.1 (0x...)"
            const size_t sp = t.find(' ');
            e.soname = t.substr(0, sp);
            if (!e.soname.empty() && e.soname[0] == '/') e.path = e.soname;
            e.soname = std::filesystem::path(e.soname).filename().string();
        } else {
            e.soname = t.substr(0, arrow);
            std::string rest = t.substr(arrow + 4);
            if (rest.rfind("not found", 0) != 0) {
                const size_t sp = rest.find(" (");
                e.path = sp == std::string::npos ? rest : rest.substr(0, sp);
            }
        }
        out.push_back(e);
    }
    return out;
}

/** @brief Desired host platform for is_system_dep(). */
enum class DepPlatform { MacOS, Linux };

/**
 * @brief True for libraries every player's machine already has (never bundled): macOS system
 *        libraries and frameworks; on Linux the C runtime, the graphics/windowing/audio stacks
 *        and the Vulkan loader, which must match the player's own drivers.
 */
inline bool is_system_dep(const std::string& path, DepPlatform platform) {
    if (platform == DepPlatform::MacOS) {
        return path.rfind("/usr/lib/", 0) == 0 || path.rfind("/System/", 0) == 0;
    }
    const std::string leaf = std::filesystem::path(path).filename().string();
    static const char* prefixes[] = {
        "ld-linux", "linux-vdso", "linux-gate", "libc.so", "libm.so", "libdl.so", "libpthread.so",
        "librt.so", "libutil.so", "libresolv.so", "libstdc++.so", "libgcc_s.so", "libGL", "libEGL",
        "libGLX", "libGLdispatch", "libOpenGL", "libX", "libxcb", "libwayland", "libxkbcommon",
        "libvulkan.so", "libasound.so", "libpulse", "libdbus", "libdrm", "libgbm", "libffi.so",
        "libsystemd", "libudev", "libz.so", "libbsd", "libmd.so", "libcap", "libexpat",
    };
    for (const char* p : prefixes) if (leaf.rfind(p, 0) == 0) return true;
    return false;
}

// =====================================================================================
// Walking a binary's dependencies
// =====================================================================================

/** @brief A non-system library to bundle: where it is now and the name it is loaded by. */
struct BundledLib {
    std::filesystem::path source;   ///< Real file (symlinks followed).
    std::string leaf;               ///< Install-name leaf the binary asks for (libglfw.3.dylib).
};

/** @brief Resolves `@rpath/x`, `@loader_path/x`, `@executable_path/x` against an image. */
inline std::optional<std::filesystem::path> resolve_macho_ref_(const std::string& ref,
                                                               const std::filesystem::path& image,
                                                               const std::filesystem::path& exe,
                                                               const std::vector<std::string>& rpaths) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto expand = [&](std::string s) -> fs::path {
        if (s.rfind("@loader_path", 0) == 0) s = image.parent_path().string() + s.substr(12);
        else if (s.rfind("@executable_path", 0) == 0) s = exe.parent_path().string() + s.substr(16);
        return fs::path(s);
    };
    if (ref.rfind("@rpath/", 0) == 0) {
        const std::string rest = ref.substr(7);
        std::vector<std::string> search = rpaths;
        search.push_back("/opt/homebrew/lib");
        search.push_back("/usr/local/lib");
        for (const std::string& rp : search) {
            const fs::path cand = expand(rp) / rest;
            if (fs::exists(cand, ec)) return cand;
        }
        return std::nullopt;
    }
    const fs::path p = expand(ref);
    if (fs::exists(p, ec)) return p;
    return std::nullopt;
}

/**
 * @brief Breadth-first walk of `binary`'s non-system dependencies. macOS: otool -L / -l;
 *        Linux: ldd (already transitive). Unresolvable references are reported in `missing`.
 */
inline std::vector<BundledLib> collect_deps(const std::filesystem::path& binary, CommandRunner& run,
                                            std::vector<std::string>* missing = nullptr) {
    namespace fs = std::filesystem;
    std::vector<BundledLib> out;
    std::set<std::string> seen_leaves;
    std::error_code ec;
#if defined(__APPLE__)
    std::deque<fs::path> queue{binary};
    std::set<std::string> visited;
    while (!queue.empty()) {
        const fs::path image = queue.front();
        queue.pop_front();
        if (!visited.insert(image.string()).second) continue;
        const CommandResult refs = run.run("otool -L " + shell_quote(image.string()), false);
        const CommandResult load = run.run("otool -l " + shell_quote(image.string()), false);
        const std::vector<std::string> rpaths = parse_otool_rpaths(load.output);
        const std::string self_leaf = image.filename().string();
        for (const std::string& ref : parse_otool_L(refs.output)) {
            if (is_system_dep(ref, DepPlatform::MacOS)) continue;
            const std::string leaf = fs::path(ref).filename().string();
            if (image != binary && leaf == self_leaf) continue;   // a dylib's own id line
            if (seen_leaves.count(leaf)) continue;
            const auto resolved = resolve_macho_ref_(ref, image, binary, rpaths);
            if (!resolved) { if (missing) missing->push_back(ref); continue; }
            seen_leaves.insert(leaf);
            const fs::path real = fs::canonical(*resolved, ec);
            out.push_back({ec ? *resolved : real, leaf});
            queue.push_back(ec ? *resolved : real);
        }
    }
#else
    const CommandResult r = run.run("ldd " + shell_quote(binary.string()), false);
    for (const LddEntry& e : parse_ldd(r.output)) {
        if (e.path.empty()) {
            if (missing && !is_system_dep(e.soname, DepPlatform::Linux)) missing->push_back(e.soname);
            continue;
        }
        if (is_system_dep(e.path, DepPlatform::Linux) || is_system_dep(e.soname, DepPlatform::Linux)) continue;
        if (!seen_leaves.insert(e.soname).second) continue;
        const fs::path real = fs::canonical(e.path, ec);
        out.push_back({ec ? fs::path(e.path) : real, e.soname});
    }
#endif
    return out;
}

/** @brief macOS: the Vulkan loader, MoltenVK and MoltenVK's ICD manifest to bundle. */
struct VulkanRuntime {
    std::filesystem::path loader;      ///< libvulkan.1.dylib
    std::filesystem::path moltenvk;    ///< libMoltenVK.dylib
    std::filesystem::path icd_json;    ///< MoltenVK_icd.json (its api_version is kept)
    bool complete() const { return !loader.empty() && !moltenvk.empty(); }
};

/** @brief Searches $VULKAN_SDK, Homebrew and /usr/local for the macOS Vulkan runtime. */
inline VulkanRuntime find_vulkan_runtime() {
    namespace fs = std::filesystem;
    VulkanRuntime vr;
    std::error_code ec;
    std::vector<fs::path> prefixes;
    if (const char* sdk = std::getenv("VULKAN_SDK"); sdk && *sdk) prefixes.emplace_back(sdk);
    for (const char* p : {"/opt/homebrew", "/opt/homebrew/opt/vulkan-loader", "/opt/homebrew/opt/molten-vk",
                          "/usr/local", "/usr/local/opt/vulkan-loader", "/usr/local/opt/molten-vk"}) {
        prefixes.emplace_back(p);
    }
    for (const fs::path& pre : prefixes) {
        if (vr.loader.empty() && fs::exists(pre / "lib" / "libvulkan.1.dylib", ec)) vr.loader = fs::canonical(pre / "lib" / "libvulkan.1.dylib", ec);
        if (vr.moltenvk.empty() && fs::exists(pre / "lib" / "libMoltenVK.dylib", ec)) vr.moltenvk = fs::canonical(pre / "lib" / "libMoltenVK.dylib", ec);
        for (const char* sub : {"etc/vulkan/icd.d", "share/vulkan/icd.d"}) {
            if (vr.icd_json.empty() && fs::exists(pre / sub / "MoltenVK_icd.json", ec)) vr.icd_json = pre / sub / "MoltenVK_icd.json";
        }
    }
    return vr;
}

/**
 * @brief MoltenVK_icd.json rewritten to point at the bundled driver: `library_path` replaced,
 *        everything else (api_version, file_format_version) kept as installed.
 */
inline std::string rewrite_icd_json(const std::string& json, const std::string& library_path) {
    const std::string key = "\"library_path\"";
    const size_t k = json.find(key);
    if (k == std::string::npos) {
        return "{\n    \"file_format_version\": \"1.0.0\",\n    \"ICD\": {\n        \"library_path\": \"" +
               library_path + "\",\n        \"api_version\": \"1.2.0\",\n        \"is_portability_driver\": true\n    }\n}\n";
    }
    const size_t q1 = json.find('"', json.find(':', k + key.size()) + 1);
    const size_t q2 = json.find('"', q1 + 1);
    if (q1 == std::string::npos || q2 == std::string::npos) return json;
    return json.substr(0, q1 + 1) + library_path + json.substr(q2);
}

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_DEPS_H
