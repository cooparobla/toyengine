/**
 * @file build_settings.h
 * @brief Per-project Build Settings: <project>/build_settings.yaml (next to the .toy, outside
 *        assets/, so it never ships). What Build needs beyond the project itself: the product's
 *        name and version, its bundle identity, signing and notarization choices.
 *
 * Signing material is referenced by name only -- a keychain identity ("Developer ID
 * Application: ...", or "auto" for the first one found) and a notarytool keychain profile
 * (created once with `xcrun notarytool store-credentials <profile>`). No secret is stored here.
 */

#ifndef TOYEDITOR_BUILD_BUILD_SETTINGS_H
#define TOYEDITOR_BUILD_BUILD_SETTINGS_H

#include "../app/project.h"

#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace toy::editor {

/** @brief Development (fast, debuggable) or Shipping (Release, hardened, signed for players). */
enum class BuildProfile { Development, Shipping };

inline const char* profile_name(BuildProfile p) { return p == BuildProfile::Shipping ? "shipping" : "development"; }

std::optional<BuildProfile> parse_profile(const std::string& s);

/** @brief The platform a build targets: always the host (there is no cross-compiling). */
const char* host_platform_name();

/** @brief Host CPU architecture, as package names use it (arm64 / x86_64). */
const char* host_arch_name();

struct BuildSettings {
    static constexpr const char* k_file_name = "build_settings.yaml";

    std::string product_name;       ///< The .app / executable name. Default: the project's name.
    std::string bundle_id;          ///< Reverse-DNS id (com.studio.game).
    std::string version = "0.1.0";  ///< CFBundleShortVersionString.
    std::string build_number = "1"; ///< CFBundleVersion.
    std::string icon_png;           ///< Project-relative square PNG (1024px ideal). Empty: toyengine's icon.
    std::string copyright;
    std::string output_dir = "build/dist";   ///< Project-relative (or absolute).
    std::string caml_key;           ///< Shipping: passphrase compiled into the game (empty = caml default).

    struct MacOS {
        std::string min_version = "13.0";        ///< LSMinimumSystemVersion.
        std::string sign_identity = "auto";      ///< "auto": first Developer ID Application identity; "-": ad-hoc.
        std::string notary_profile;              ///< notarytool keychain profile; empty skips notarization.
        bool notarize = true;
        bool dmg = false;
        std::string entitlements;                ///< Project-relative .plist; empty: the engine's default.
    } macos;

    struct Linux {
        bool tarball = true;
    } linux_;

    /** @brief A filesystem/bundle-safe version of `s` (letters, digits, '-', '.', '_'). */
    static std::string sanitize(const std::string& s, bool allow_space = true);

    /** @brief Defaults for `project`: its name, a com.<user>.<name> bundle id. */
    static BuildSettings defaults_for(const Project& project);

    static std::filesystem::path path_for(const Project& project) { return project.root() / k_file_name; }

    /** @brief Loads the project's settings over its defaults (missing file: defaults). */
    static BuildSettings load(const Project& project);

    void save(const Project& project) const { coopa::yaml::save_document(path_for(project), to_node()); }

    fkyaml::node to_node() const;

    /** @brief The output directory, resolved against the project root. */
    std::filesystem::path output_path(const Project& project) const;

    /** @brief Product name safe for a file name. */
    std::string file_name() const { return sanitize(product_name); }

private:
    static void str_(const fkyaml::node& n, const char* key, std::string& out);
    static void bool_(const fkyaml::node& n, const char* key, bool& out) {
        if (n.contains(key) && n[key].is_boolean()) out = n[key].get_value<bool>();
    }
    void apply_(const fkyaml::node& n);
};

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_BUILD_SETTINGS_H
