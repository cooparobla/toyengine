/**
 * @file bundle_macos.h
 * @brief Turns a staged game into a self-contained, signed macOS .app (and, for Shipping, a
 *        notarized, stapled .zip / .dmg).
 *
 *   <Product>.app/Contents/
 *     Info.plist
 *     MacOS/<Product>                       the game executable
 *     Frameworks/*.dylib                    GLFW, libcrypto, ..., libvulkan, libMoltenVK
 *     Resources/toy_package.yaml            marks the packaged layout (runtime_paths.h)
 *     Resources/assets/...                  staged by package_project()
 *     Resources/<Product>.icns
 *     Resources/vulkan/icd.d/MoltenVK_icd.json   -> ../../../Frameworks/libMoltenVK.dylib
 *
 * Every bundled library is renamed @rpath/<leaf>, every reference to it rewritten, the
 * executable's absolute rpaths (/opt/homebrew/lib) deleted and @executable_path/../Frameworks
 * added -- then every Mach-O is re-signed, inside out, because install_name_tool invalidates a
 * signature and arm64 macOS will not run unsigned code.
 */

#ifndef TOYEDITOR_BUILD_BUNDLE_MACOS_H
#define TOYEDITOR_BUILD_BUNDLE_MACOS_H

#include "build_settings.h"
#include "deps.h"

#include <toyengine/core/runtime_paths.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <root_directory.h>

namespace toy::editor {

/** @brief Inputs shared by the platform bundlers. */
struct BundleJob {
    BuildSettings settings;
    BuildProfile profile = BuildProfile::Development;
    std::filesystem::path project_root;
    std::filesystem::path exe;          ///< The compiled game binary.
};

/** @brief What a platform bundler produced. */
struct BundleResult {
    bool ok = false;
    std::filesystem::path artifact;     ///< The .app / folder.
    std::filesystem::path archive;      ///< The .zip / .dmg / .tar.gz (Shipping), if any.
    std::string signed_with;            ///< "ad-hoc" or the identity.
    bool notarized = false;
    std::vector<std::string> warnings;
    std::string error;
};

/** @brief The .app path and its subdirectories for `name` under `out_dir`. */
struct MacAppLayout {
    std::filesystem::path app, contents, macos, frameworks, resources;
    static MacAppLayout for_(const std::filesystem::path& out_dir, const std::string& name);
};

/** @brief Escapes text for a plist <string>. */
std::string plist_escape(const std::string& s);

/** @brief The game's Info.plist. */
std::string make_info_plist(const BuildSettings& s, const std::string& exe_name,
                                   const std::string& icon_name, const std::string& min_os);

/** @brief The default (empty) entitlements: a game signed by one team needs none. */
std::string default_entitlements_plist();

/** @brief Compares dotted versions ("13.0" < "14.2"). */
int compare_versions(const std::string& a, const std::string& b);

/**
 * @brief The codesigning identity `requested` means: "-" ad-hoc; "auto" the first valid
 *        "Developer ID Application" identity in the keychain (ad-hoc if none); else as given.
 */
std::string resolve_sign_identity(const std::string& requested, CommandRunner& run);

/** @brief Lists the valid codesigning identities' names (Build Settings' Detect button). */
std::vector<std::string> list_sign_identities(CommandRunner& run);

namespace detail {

bool fail_(BundleResult& r, const std::string& msg);

bool must_(CommandRunner& run, const std::string& cmd, BundleResult& r);

/** @brief Builds Resources/<name>.icns from a square PNG with sips + iconutil. */
bool make_icns_(const std::filesystem::path& png, const std::filesystem::path& icns,
                       CommandRunner& run, BundleResult& r);

} // namespace detail

/**
 * @brief Finishes a .app whose Resources/ already holds the staged assets and manifest:
 *        executable, frameworks, Vulkan runtime, Info.plist, icon, signing; then, for
 *        Shipping, notarization and the archive.
 */
BundleResult finalize_macos_app(const BundleJob& job, const MacAppLayout& l, CommandRunner& run);

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_BUNDLE_MACOS_H
