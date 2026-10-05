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

inline std::optional<BuildProfile> parse_profile(const std::string& s) {
    if (s == "dev" || s == "development") return BuildProfile::Development;
    if (s == "ship" || s == "shipping") return BuildProfile::Shipping;
    return std::nullopt;
}

/** @brief The platform a build targets: always the host (there is no cross-compiling). */
inline const char* host_platform_name() {
#if defined(__APPLE__)
    return "macOS";
#else
    return "Linux";
#endif
}

/** @brief Host CPU architecture, as package names use it (arm64 / x86_64). */
inline const char* host_arch_name() {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#else
    return "x86_64";
#endif
}

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
    static std::string sanitize(const std::string& s, bool allow_space = true) {
        std::string out;
        for (char c : s) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') out += c;
            else if (c == ' ' && allow_space) out += c;
            else out += '_';
        }
        return out.empty() ? std::string("Game") : out;
    }

    /** @brief Defaults for `project`: its name, a com.<user>.<name> bundle id. */
    static BuildSettings defaults_for(const Project& project) {
        BuildSettings s;
        s.product_name = project.name().empty() ? std::string("Game") : project.name();
        std::string user = std::getenv("USER") ? std::getenv("USER") : "toyengine";
        std::string id_name;
        for (char c : s.product_name) {
            if (std::isalnum(static_cast<unsigned char>(c))) id_name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        std::string id_user;
        for (char c : user) {
            if (std::isalnum(static_cast<unsigned char>(c))) id_user += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        s.bundle_id = "com." + (id_user.empty() ? std::string("toyengine") : id_user) + "." +
                      (id_name.empty() ? std::string("game") : id_name);
        return s;
    }

    static std::filesystem::path path_for(const Project& project) { return project.root() / k_file_name; }

    /** @brief Loads the project's settings over its defaults (missing file: defaults). */
    static BuildSettings load(const Project& project) {
        BuildSettings s = defaults_for(project);
        std::optional<fkyaml::node> doc;
        try { doc = coopa::yaml::try_load_document(path_for(project)); } catch (...) { return s; }
        if (doc && doc->is_mapping()) s.apply_(*doc);
        return s;
    }

    void save(const Project& project) const { coopa::yaml::save_document(path_for(project), to_node()); }

    fkyaml::node to_node() const {
        using N = fkyaml::node;
        N n = N::mapping();
        n["product_name"] = N(product_name);
        n["bundle_id"] = N(bundle_id);
        n["version"] = N(version);
        n["build_number"] = N(build_number);
        n["icon_png"] = N(icon_png);
        n["copyright"] = N(copyright);
        n["output_dir"] = N(output_dir);
        n["caml_key"] = N(caml_key);
        N mac = N::mapping();
        mac["min_version"] = N(macos.min_version);
        mac["sign_identity"] = N(macos.sign_identity);
        mac["notary_profile"] = N(macos.notary_profile);
        mac["notarize"] = N(macos.notarize);
        mac["dmg"] = N(macos.dmg);
        mac["entitlements"] = N(macos.entitlements);
        n["macos"] = mac;
        N lin = N::mapping();
        lin["tarball"] = N(linux_.tarball);
        n["linux"] = lin;
        return n;
    }

    /** @brief The output directory, resolved against the project root. */
    std::filesystem::path output_path(const Project& project) const {
        const std::filesystem::path p(output_dir.empty() ? std::string("build/dist") : output_dir);
        return p.is_absolute() ? p : project.root() / p;
    }

    /** @brief Product name safe for a file name. */
    std::string file_name() const { return sanitize(product_name); }

private:
    static void str_(const fkyaml::node& n, const char* key, std::string& out) {
        if (n.contains(key)) {
            const fkyaml::node& v = n[key];
            if (v.is_string()) out = v.get_value<std::string>();
            else if (v.is_integer()) out = std::to_string(v.get_value<int64_t>());
            else if (v.is_float_number()) out = coopa::yaml::detail::format_float(v.get_value<double>());
        }
    }
    static void bool_(const fkyaml::node& n, const char* key, bool& out) {
        if (n.contains(key) && n[key].is_boolean()) out = n[key].get_value<bool>();
    }
    void apply_(const fkyaml::node& n) {
        str_(n, "product_name", product_name);
        str_(n, "bundle_id", bundle_id);
        str_(n, "version", version);
        str_(n, "build_number", build_number);
        str_(n, "icon_png", icon_png);
        str_(n, "copyright", copyright);
        str_(n, "output_dir", output_dir);
        str_(n, "caml_key", caml_key);
        if (n.contains("macos") && n["macos"].is_mapping()) {
            const fkyaml::node& m = n["macos"];
            str_(m, "min_version", macos.min_version);
            str_(m, "sign_identity", macos.sign_identity);
            str_(m, "notary_profile", macos.notary_profile);
            bool_(m, "notarize", macos.notarize);
            bool_(m, "dmg", macos.dmg);
            str_(m, "entitlements", macos.entitlements);
        }
        if (n.contains("linux") && n["linux"].is_mapping()) bool_(n["linux"], "tarball", linux_.tarball);
        if (product_name.empty()) product_name = "Game";
    }
};

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_BUILD_SETTINGS_H
