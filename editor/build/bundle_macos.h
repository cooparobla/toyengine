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
    static MacAppLayout for_(const std::filesystem::path& out_dir, const std::string& name) {
        MacAppLayout l;
        l.app = out_dir / (name + ".app");
        l.contents = l.app / "Contents";
        l.macos = l.contents / "MacOS";
        l.frameworks = l.contents / "Frameworks";
        l.resources = l.contents / "Resources";
        return l;
    }
};

/** @brief Escapes text for a plist <string>. */
inline std::string plist_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else o += c;
    }
    return o;
}

/** @brief The game's Info.plist. */
inline std::string make_info_plist(const BuildSettings& s, const std::string& exe_name,
                                   const std::string& icon_name, const std::string& min_os) {
    std::map<std::string, std::string> strings = {
        {"CFBundleName", s.product_name},
        {"CFBundleDisplayName", s.product_name},
        {"CFBundleIdentifier", s.bundle_id},
        {"CFBundleExecutable", exe_name},
        {"CFBundleIconFile", icon_name},
        {"CFBundlePackageType", "APPL"},
        {"CFBundleShortVersionString", s.version},
        {"CFBundleVersion", s.build_number},
        {"CFBundleInfoDictionaryVersion", "6.0"},
        {"CFBundleDevelopmentRegion", "en"},
        {"LSMinimumSystemVersion", min_os},
        {"LSApplicationCategoryType", "public.app-category.games"},
    };
    if (!s.copyright.empty()) strings["NSHumanReadableCopyright"] = s.copyright;
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
         "<plist version=\"1.0\">\n<dict>\n";
    for (const auto& [k, v] : strings) {
        o << "    <key>" << k << "</key>\n    <string>" << plist_escape(v) << "</string>\n";
    }
    o << "    <key>NSHighResolutionCapable</key>\n    <true/>\n"
         "    <key>NSSupportsAutomaticGraphicsSwitching</key>\n    <true/>\n"
         "</dict>\n</plist>\n";
    return o.str();
}

/** @brief The default (empty) entitlements: a game signed by one team needs none. */
inline std::string default_entitlements_plist() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\">\n<dict>\n</dict>\n</plist>\n";
}

/** @brief Compares dotted versions ("13.0" < "14.2"). */
inline int compare_versions(const std::string& a, const std::string& b) {
    std::istringstream ia(a), ib(b);
    for (;;) {
        std::string pa, pb;
        const bool ga = static_cast<bool>(std::getline(ia, pa, '.'));
        const bool gb = static_cast<bool>(std::getline(ib, pb, '.'));
        if (!ga && !gb) return 0;
        const int na = ga ? std::atoi(pa.c_str()) : 0, nb = gb ? std::atoi(pb.c_str()) : 0;
        if (na != nb) return na < nb ? -1 : 1;
    }
}

/**
 * @brief The codesigning identity `requested` means: "-" ad-hoc; "auto" the first valid
 *        "Developer ID Application" identity in the keychain (ad-hoc if none); else as given.
 */
inline std::string resolve_sign_identity(const std::string& requested, CommandRunner& run) {
    if (requested.empty() || requested == "-") return "-";
    if (requested != "auto") return requested;
    const CommandResult r = run.run("security find-identity -v -p codesigning", false);
    std::istringstream in(r.output);
    std::string line;
    while (std::getline(in, line)) {
        const size_t q1 = line.find('"');
        const size_t q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
        if (q2 == std::string::npos) continue;
        const std::string name = line.substr(q1 + 1, q2 - q1 - 1);
        if (name.rfind("Developer ID Application", 0) == 0) return name;
    }
    return "-";
}

/** @brief Lists the valid codesigning identities' names (Build Settings' Detect button). */
inline std::vector<std::string> list_sign_identities(CommandRunner& run) {
    std::vector<std::string> out;
    const CommandResult r = run.run("security find-identity -v -p codesigning", false);
    std::istringstream in(r.output);
    std::string line;
    while (std::getline(in, line)) {
        const size_t q1 = line.find('"');
        const size_t q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
        if (q2 != std::string::npos) out.push_back(line.substr(q1 + 1, q2 - q1 - 1));
    }
    return out;
}

namespace detail {

inline bool fail_(BundleResult& r, const std::string& msg) {
    r.ok = false;
    r.error = msg;
    return false;
}

inline bool must_(CommandRunner& run, const std::string& cmd, BundleResult& r) {
    const CommandResult c = run.run(cmd);
    if (!c.ok()) return fail_(r, "command failed (" + std::to_string(c.exit_code) + "): " + cmd);
    return true;
}

/** @brief Builds Resources/<name>.icns from a square PNG with sips + iconutil. */
inline bool make_icns_(const std::filesystem::path& png, const std::filesystem::path& icns,
                       CommandRunner& run, BundleResult& r) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path iconset = icns.parent_path() / "icon.iconset";
    fs::remove_all(iconset, ec);
    fs::create_directories(iconset, ec);
    for (int size : {16, 32, 128, 256, 512}) {
        for (int scale : {1, 2}) {
            const int px = size * scale;
            const std::string name = "icon_" + std::to_string(size) + "x" + std::to_string(size) +
                                     (scale == 2 ? "@2x" : "") + ".png";
            if (!must_(run, "sips -z " + std::to_string(px) + " " + std::to_string(px) + " " +
                                shell_quote(png.string()) + " --out " + shell_quote((iconset / name).string()) + " >/dev/null",
                       r)) return false;
        }
    }
    const bool ok = must_(run, "iconutil -c icns " + shell_quote(iconset.string()) + " -o " + shell_quote(icns.string()), r);
    fs::remove_all(iconset, ec);
    return ok;
}

} // namespace detail

/**
 * @brief Finishes a .app whose Resources/ already holds the staged assets and manifest:
 *        executable, frameworks, Vulkan runtime, Info.plist, icon, signing; then, for
 *        Shipping, notarization and the archive.
 */
inline BundleResult finalize_macos_app(const BundleJob& job, const MacAppLayout& l, CommandRunner& run) {
    namespace fs = std::filesystem;
    BundleResult r;
    r.artifact = l.app;
    std::error_code ec;
    const std::string exe_name = job.settings.file_name();
    const fs::path exe = l.macos / exe_name;
    fs::create_directories(l.macos, ec);
    fs::create_directories(l.frameworks, ec);
    fs::create_directories(l.resources, ec);

    // --- Executable ---
    fs::copy_file(job.exe, exe, fs::copy_options::overwrite_existing, ec);
    if (ec) { detail::fail_(r, "copy " + job.exe.string() + ": " + ec.message()); return r; }
    fs::permissions(exe, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                         fs::perms::others_read | fs::perms::others_exec, ec);

    // --- Dependencies (+ the Vulkan runtime volk dlopen()s, which otool can't see) ---
    run.log("Collecting libraries...");
    std::vector<std::string> missing;
    std::vector<BundledLib> libs = collect_deps(job.exe, run, &missing);
    for (const std::string& m : missing) r.warnings.push_back("unresolved library reference: " + m);
    const VulkanRuntime vk = find_vulkan_runtime();
    if (!vk.complete()) {
        detail::fail_(r, "Vulkan runtime not found (need libvulkan.1.dylib and libMoltenVK.dylib: brew install vulkan-loader molten-vk)");
        return r;
    }
    libs.push_back({vk.loader, "libvulkan.1.dylib"});
    libs.push_back({vk.moltenvk, "libMoltenVK.dylib"});
    std::set<std::string> leaves;
    for (const BundledLib& lib : libs) {
        if (!leaves.insert(lib.leaf).second) continue;
        const fs::path dst = l.frameworks / lib.leaf;
        fs::copy_file(lib.source, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) { detail::fail_(r, "copy " + lib.source.string() + ": " + ec.message()); return r; }
        fs::permissions(dst, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read, ec);
        run.log("  bundled " + lib.leaf + "  <-  " + lib.source.string());
    }

    // --- Rewrite install names: every bundled lib is @rpath/<leaf> ---
    std::vector<fs::path> images{exe};
    for (const std::string& leaf : leaves) images.push_back(l.frameworks / leaf);
    for (const fs::path& image : images) {
        const bool is_exe = image == exe;
        std::string cmd = "install_name_tool";
        bool any = false;
        if (!is_exe) { cmd += " -id " + shell_quote("@rpath/" + image.filename().string()); any = true; }
        bool refs_bundled = false;
        for (const std::string& ref : parse_otool_L(run.run("otool -L " + shell_quote(image.string()), false).output)) {
            const std::string leaf = fs::path(ref).filename().string();
            if (!leaves.count(leaf) || (!is_exe && leaf == image.filename().string())) continue;
            const std::string target = "@rpath/" + leaf;
            if (ref != target) { cmd += " -change " + shell_quote(ref) + " " + shell_quote(target); any = true; }
            refs_bundled = true;
        }
        const std::vector<std::string> rpaths = parse_otool_rpaths(run.run("otool -l " + shell_quote(image.string()), false).output);
        for (const std::string& rp : rpaths) {
            if (rp.rfind('@', 0) != 0) { cmd += " -delete_rpath " + shell_quote(rp); any = true; }
        }
        const std::string want_rpath = is_exe ? "@executable_path/../Frameworks" : "@loader_path";
        if ((is_exe || refs_bundled) && std::find(rpaths.begin(), rpaths.end(), want_rpath) == rpaths.end()) {
            cmd += " -add_rpath " + shell_quote(want_rpath);
            any = true;
        }
        if (any && !detail::must_(run, cmd + " " + shell_quote(image.string()), r)) return r;
    }

    // --- Verify: nothing still points outside the bundle ---
    std::string min_os = job.settings.macos.min_version;
    for (const fs::path& image : images) {
        for (const std::string& ref : parse_otool_L(run.run("otool -L " + shell_quote(image.string()), false).output)) {
            if (is_system_dep(ref, DepPlatform::MacOS) || ref.rfind("@rpath/", 0) == 0) continue;
            if (image != exe && fs::path(ref).filename() == image.filename()) continue;
            detail::fail_(r, image.filename().string() + " still references " + ref);
            return r;
        }
        const std::string load = run.run("otool -l " + shell_quote(image.string()), false).output;
        for (const std::string& rp : parse_otool_rpaths(load)) {
            if (rp.rfind('@', 0) != 0) { detail::fail_(r, image.filename().string() + " still has rpath " + rp); return r; }
        }
        const std::string minos = parse_otool_minos(load);
        if (!minos.empty() && compare_versions(minos, min_os) > 0) {
            r.warnings.push_back(image.filename().string() + " requires macOS " + minos + "; raising LSMinimumSystemVersion from " + min_os);
            min_os = minos;
        }
    }

    // --- MoltenVK ICD manifest, pointing at the bundled driver ---
    {
        const fs::path icd_dir = l.resources / "vulkan" / "icd.d";
        fs::create_directories(icd_dir, ec);
        std::string json;
        if (!vk.icd_json.empty()) {
            std::ifstream in(vk.icd_json);
            json.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::ofstream(icd_dir / "MoltenVK_icd.json") << rewrite_icd_json(json, "../../../Frameworks/libMoltenVK.dylib");
    }

    // --- Icon + Info.plist ---
    const std::string icon_name = exe_name + ".icns";
    if (!job.settings.icon_png.empty()) {
        fs::path png(job.settings.icon_png);
        if (png.is_relative()) png = job.project_root / png;
        if (!fs::exists(png, ec)) { detail::fail_(r, "icon not found: " + png.string()); return r; }
        if (!detail::make_icns_(png, l.resources / icon_name, run, r)) return r;
    } else {
        fs::copy_file(fs::path(ROOT_DIR) / "editor" / "branding" / "toyengine.icns", l.resources / icon_name,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) r.warnings.push_back("no icon: " + ec.message());
    }
    std::ofstream(l.contents / "Info.plist") << make_info_plist(job.settings, exe_name, icon_name, min_os);
    std::ofstream(l.contents / "PkgInfo") << "APPL????";

    // --- Sign, inside out ---
    const bool shipping = job.profile == BuildProfile::Shipping;
    const std::string identity = shipping ? resolve_sign_identity(job.settings.macos.sign_identity, run) : "-";
    const bool adhoc = identity == "-";
    if (shipping && adhoc) {
        r.warnings.push_back("no Developer ID identity found -- signed ad-hoc; the app will run here but Gatekeeper "
                             "will block it on other Macs (set macos.sign_identity in Build Settings)");
    }
    r.signed_with = adhoc ? "ad-hoc" : identity;
    fs::path entitlements = l.app.parent_path() / ".entitlements.plist";
    if (!job.settings.macos.entitlements.empty()) {
        entitlements = job.settings.macos.entitlements;
        if (entitlements.is_relative()) entitlements = job.project_root / entitlements;
    } else {
        std::ofstream(entitlements) << default_entitlements_plist();
    }
    // Hardened runtime only with a real identity: an ad-hoc signature has no team, so library
    // validation would refuse to load the bundled dylibs.
    const std::string sign_base = "codesign --force --sign " + shell_quote(identity) +
                                  (adhoc ? std::string() : std::string(" --timestamp --options runtime"));
    for (const std::string& leaf : leaves) {
        if (!detail::must_(run, sign_base + " " + shell_quote((l.frameworks / leaf).string()), r)) return r;
    }
    if (!detail::must_(run, sign_base + " --entitlements " + shell_quote(entitlements.string()) + " " + shell_quote(l.app.string()), r)) return r;
    if (job.settings.macos.entitlements.empty()) fs::remove(entitlements, ec);
    if (!detail::must_(run, "codesign --verify --deep --strict " + shell_quote(l.app.string()), r)) return r;

    if (!shipping) { r.ok = true; return r; }

    // --- Shipping: notarize, staple, archive ---
    const std::string base = exe_name + "-" + job.settings.version + "-macos-" + host_arch_name();
    const fs::path zip = l.app.parent_path() / (base + ".zip");
    const auto make_zip = [&] {
        fs::remove(zip, ec);
        return detail::must_(run, "ditto -c -k --sequesterRsrc --keepParent " + shell_quote(l.app.string()) + " " + shell_quote(zip.string()), r);
    };
    const bool want_notary = !adhoc && job.settings.macos.notarize && !job.settings.macos.notary_profile.empty();
    if (!adhoc && job.settings.macos.notarize && job.settings.macos.notary_profile.empty()) {
        r.warnings.push_back("not notarized: set macos.notary_profile (xcrun notarytool store-credentials <profile>)");
    }
    if (want_notary) {
        if (!make_zip()) return r;
        run.log("Submitting to Apple's notary service (this can take a few minutes)...");
        const CommandResult sub = run.run("xcrun notarytool submit " + shell_quote(zip.string()) +
                                          " --keychain-profile " + shell_quote(job.settings.macos.notary_profile) + " --wait");
        if (!sub.ok() || sub.output.find("status: Accepted") == std::string::npos) {
            const size_t id_at = sub.output.find("id: ");
            if (id_at != std::string::npos) {
                const std::string id = trim_(sub.output.substr(id_at + 4, sub.output.find('\n', id_at) - id_at - 4));
                run.run("xcrun notarytool log " + shell_quote(id) + " --keychain-profile " + shell_quote(job.settings.macos.notary_profile));
            }
            detail::fail_(r, "notarization was not accepted (see the log above)");
            return r;
        }
        if (!detail::must_(run, "xcrun stapler staple " + shell_quote(l.app.string()), r)) return r;
        run.run("spctl -a -vv " + shell_quote(l.app.string()));
        r.notarized = true;
    }
    if (!make_zip()) return r;
    r.archive = zip;
    if (job.settings.macos.dmg) {
        const fs::path dmg = l.app.parent_path() / (base + ".dmg");
        fs::remove(dmg, ec);
        if (!detail::must_(run, "hdiutil create -volname " + shell_quote(job.settings.product_name) + " -srcfolder " +
                                    shell_quote(l.app.string()) + " -ov -format UDZO " + shell_quote(dmg.string()), r)) return r;
        if (!adhoc && !detail::must_(run, "codesign --force --timestamp --sign " + shell_quote(identity) + " " + shell_quote(dmg.string()), r)) return r;
        if (want_notary) {
            const CommandResult sub = run.run("xcrun notarytool submit " + shell_quote(dmg.string()) +
                                              " --keychain-profile " + shell_quote(job.settings.macos.notary_profile) + " --wait");
            if (sub.ok() && sub.output.find("status: Accepted") != std::string::npos) {
                run.run("xcrun stapler staple " + shell_quote(dmg.string()));
            } else {
                r.warnings.push_back("the .dmg was not notarized (the .zip is)");
            }
        }
        r.archive = dmg;
    }
    r.ok = true;
    return r;
}

} // namespace toy::editor

#endif // TOYEDITOR_BUILD_BUNDLE_MACOS_H
