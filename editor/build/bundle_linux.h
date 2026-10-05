/**
 * @file bundle_linux.h
 * @brief Turns a staged game into a relocatable Linux folder (and, for Shipping, a .tar.gz).
 *
 *   <Product>/
 *     <Product>            the game executable (DT_RPATH $ORIGIN/lib, set at link time --
 *                          cmake/ToyProject.cmake -- so bundled libraries' own dependencies
 *                          resolve there too; no patchelf needed)
 *     <Product>.sh         launcher: checks for a system Vulkan loader, then execs the game
 *     toy_package.yaml     marks the packaged layout (runtime_paths.h)
 *     assets/...           staged by package_project()
 *     lib/*.so*            non-system libraries (GLFW, libcrypto, ...)
 *
 * The Vulkan loader is never bundled on Linux: it must match the player's GPU drivers.
 */

#ifndef TOYEDITOR_BUILD_BUNDLE_LINUX_H
#define TOYEDITOR_BUILD_BUNDLE_LINUX_H

#include "bundle_macos.h"   // BundleJob, BundleResult
#include "deps.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace toy::editor {

/** @brief The launcher script for `exe_name`. */
inline std::string make_linux_launcher(const std::string& exe_name) {
    return "#!/bin/sh\n"
           "# Launches " + exe_name + " from wherever this folder was unpacked.\n"
           "HERE=\"$(cd \"$(dirname \"$0\")\" && pwd)\"\n"
           "if command -v ldconfig >/dev/null 2>&1 && ! ldconfig -p 2>/dev/null | grep -q 'libvulkan.so.1'; then\n"
           "  echo \"" + exe_name + " needs Vulkan: install your distribution's Vulkan loader and GPU driver \"\\\n"
           "       \"(e.g. libvulkan1 + mesa-vulkan-drivers, or the NVIDIA driver).\" >&2\n"
           "  exit 1\n"
           "fi\n"
           "exec \"$HERE/" + exe_name + "\" \"$@\"\n";
}

/** @brief Finishes a folder whose assets/ and manifest are already staged at `root`. */
inline BundleResult finalize_linux_folder(const BundleJob& job, const std::filesystem::path& root, CommandRunner& run) {
    namespace fs = std::filesystem;
    BundleResult r;
    r.artifact = root;
    std::error_code ec;
    const std::string exe_name = job.settings.file_name();
    const fs::path exe = root / exe_name;
    fs::copy_file(job.exe, exe, fs::copy_options::overwrite_existing, ec);
    if (ec) { r.error = "copy " + job.exe.string() + ": " + ec.message(); return r; }
    fs::permissions(exe, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                         fs::perms::others_read | fs::perms::others_exec, ec);
    if (job.profile == BuildProfile::Shipping) run.run("strip --strip-unneeded " + shell_quote(exe.string()));

    run.log("Collecting libraries...");
    std::vector<std::string> missing;
    const std::vector<BundledLib> libs = collect_deps(job.exe, run, &missing);
    for (const std::string& m : missing) r.warnings.push_back("unresolved library: " + m);
    const fs::path lib_dir = root / "lib";
    fs::create_directories(lib_dir, ec);
    for (const BundledLib& lib : libs) {
        fs::copy_file(lib.source, lib_dir / lib.leaf, fs::copy_options::overwrite_existing, ec);
        if (ec) { r.error = "copy " + lib.source.string() + ": " + ec.message(); return r; }
        run.log("  bundled " + lib.leaf + "  <-  " + lib.source.string());
    }

    const fs::path launcher = root / (exe_name + ".sh");
    std::ofstream(launcher) << make_linux_launcher(exe_name);
    fs::permissions(launcher, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec, ec);

    // Verify: every non-system library resolves inside lib/.
    const CommandResult ldd = run.run("ldd " + shell_quote(exe.string()), false);
    for (const LddEntry& e : parse_ldd(ldd.output)) {
        if (is_system_dep(e.soname, DepPlatform::Linux) || is_system_dep(e.path, DepPlatform::Linux)) continue;
        if (e.path.empty()) { r.error = e.soname + " does not resolve (missing from lib/)"; return r; }
        if (fs::path(e.path).parent_path() != fs::weakly_canonical(lib_dir, ec) &&
            fs::path(e.path).parent_path() != lib_dir) {
            r.error = e.soname + " resolves outside the package: " + e.path + " (is the $ORIGIN/lib rpath set?)";
            return r;
        }
    }

    if (job.profile == BuildProfile::Shipping && job.settings.linux_.tarball) {
        const fs::path tgz = root.parent_path() / (exe_name + "-" + job.settings.version + "-linux-" + host_arch_name() + ".tar.gz");
        fs::remove(tgz, ec);
        const CommandResult t = run.run("tar -czf " + shell_quote(tgz.string()) + " -C " + shell_quote(root.parent_path().string()) +
                                        " " + shell_quote(root.filename().string()));
        if (!t.ok()) { r.error = "tar failed"; return r; }
        r.archive = tgz;
    }
    r.signed_with = "n/a";
    r.ok = true;
    return r;
}

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_BUNDLE_LINUX_H
