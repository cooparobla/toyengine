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
std::string make_linux_launcher(const std::string& exe_name);

/** @brief Finishes a folder whose assets/ and manifest are already staged at `root`. */
BundleResult finalize_linux_folder(const BundleJob& job, const std::filesystem::path& root, CommandRunner& run);

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_BUNDLE_LINUX_H
