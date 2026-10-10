/**
 * @file build_pipeline_test.cpp
 * @brief Build > Build's inputs: build_settings.yaml round-trips outside assets/, profile names parse,
 * and the otool / ldd / ICD-manifest parsers that decide what a package bundles.
 */

#include <coopa/testing/test.h>

#include "editor/app/project.h"
#include "editor/build/build_pipeline.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("build_pipeline");

namespace toy::editor::testing {

COOPA_TEST(settings_round_trip_outside_assets) {
    use_scratch_home();
    const fs::path root = coopa::test::scratch_dir("build_settings");
    Project project = Project::create(root);
    BuildSettings d = BuildSettings::load(project);
    expect(d.product_name == project.name() && d.bundle_id.rfind("com.", 0) == 0, "defaults: the project's name, a com.* bundle id");
    d.version = "1.2.3";
    d.macos.sign_identity = "Developer ID Application: Example (TEAM123)";
    d.macos.notary_profile = "toy-notary";
    d.macos.dmg = true;
    d.linux_.tarball = false;
    d.save(project);
    const BuildSettings r = BuildSettings::load(project);
    expect(r.version == "1.2.3" && r.macos.sign_identity == d.macos.sign_identity && r.macos.notary_profile == "toy-notary" &&
           r.macos.dmg && !r.linux_.tarball, "build_settings.yaml round-trips");
    expect(fs::exists(root / "build_settings.yaml") && !fs::exists(project.assets() / "build_settings.yaml"),
           "build settings live outside assets/ (never shipped)");
    expect(parse_profile("dev") == BuildProfile::Development && parse_profile("ship") == BuildProfile::Shipping &&
           !parse_profile("release"), "profile names parse");
}

COOPA_TEST(otool_ldd_and_icd_parsers) {
    const std::string otool_L =
        "build/game:\n"
        "\t/opt/homebrew/opt/glfw/lib/libglfw.3.dylib (compatibility version 3.0.0, current version 3.4.0)\n"
        "\t/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit (compatibility version 45.0.0)\n"
        "\t@rpath/libcrypto.3.dylib (compatibility version 3.0.0, current version 3.0.0)\n"
        "\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n";
    const auto refs = parse_otool_L(otool_L);
    expect(refs.size() == 4 && refs[0] == "/opt/homebrew/opt/glfw/lib/libglfw.3.dylib" && refs[2] == "@rpath/libcrypto.3.dylib",
           "otool -L install names parse");
    expect(!is_system_dep(refs[0], DepPlatform::MacOS) && is_system_dep(refs[1], DepPlatform::MacOS) &&
           is_system_dep(refs[3], DepPlatform::MacOS), "Homebrew libraries bundle, system ones don't");
    const std::string otool_l =
        "Load command 12\n          cmd LC_RPATH\n      cmdsize 32\n         path /opt/homebrew/lib (offset 12)\n"
        "Load command 13\n      cmd LC_BUILD_VERSION\n  cmdsize 32\n platform 1\n    minos 14.0\n      sdk 14.5\n";
    const auto rp = parse_otool_rpaths(otool_l);
    expect(rp.size() == 1 && rp[0] == "/opt/homebrew/lib", "LC_RPATH entries parse");
    expect(parse_otool_minos(otool_l) == "14.0", "the minimum macOS parses");
    expect(compare_versions("13.0", "14.0") < 0 && compare_versions("14.2", "14.10") < 0 && compare_versions("14", "14.0") == 0,
           "dotted versions compare numerically");
    const std::string ldd =
        "\tlinux-vdso.so.1 (0x00007ffd)\n"
        "\tlibglfw.so.3 => /usr/lib/x86_64-linux-gnu/libglfw.so.3 (0x00007f)\n"
        "\tlibvulkan.so.1 => /lib/x86_64-linux-gnu/libvulkan.so.1 (0x00007f)\n"
        "\tlibmissing.so.2 => not found\n"
        "\t/lib64/ld-linux-x86-64.so.2 (0x00007f)\n";
    const auto e = parse_ldd(ldd);
    expect(e.size() == 5 && e[1].soname == "libglfw.so.3" && e[1].path == "/usr/lib/x86_64-linux-gnu/libglfw.so.3" &&
           e[3].path.empty() && e[4].path == "/lib64/ld-linux-x86-64.so.2", "ldd lines parse");
    expect(!is_system_dep(e[1].path, DepPlatform::Linux) && is_system_dep(e[2].path, DepPlatform::Linux) &&
           is_system_dep(e[0].soname, DepPlatform::Linux), "Linux: GLFW bundles, the Vulkan loader and C runtime never do");
    const std::string icd = "{\n  \"ICD\": {\n    \"library_path\": \"../../../lib/libMoltenVK.dylib\",\n    \"api_version\": \"1.4.0\"\n  }\n}\n";
    const std::string out = rewrite_icd_json(icd, "../../../Frameworks/libMoltenVK.dylib");
    expect(out.find("\"../../../Frameworks/libMoltenVK.dylib\"") != std::string::npos && out.find("1.4.0") != std::string::npos,
           "the ICD manifest points at the bundled driver and keeps its api_version");
}

} // namespace toy::editor::testing
