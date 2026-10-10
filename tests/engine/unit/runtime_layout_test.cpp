/**
 * @file runtime_layout_test.cpp
 * @brief Where a game finds its files (toyengine/core/runtime_paths.h): packaged folder and .app
 *        detection by manifest, the from-source fallback to compiled shaders (never a source dir),
 *        and per-user directories under HOME.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <string>

#include <toyengine/core/runtime_paths.h>

#include "engine/support/checks.h"
#include "engine/support/scratch_files.h"

COOPA_TEST_SUITE("runtime_layout");

using namespace toy::test;

COOPA_TEST(folder_package_is_detected_by_its_manifest) {
    namespace fs = std::filesystem;
    const fs::path root = coopa::test::scratch_dir("layout_folder");
    write_text_file(root / "game", "");
    toy::core::PackageManifest m;
    m.name = "Game";
    m.bundle_id = "com.example.game";
    m.profile = "shipping";
    m.save(root / toy::core::PackageManifest::k_file_name);
    const auto l = toy::core::RuntimeLayout::detect(root / "game");
    expect(l.packaged() && l.resources_root == root, "a toy_package.yaml beside the executable marks a packaged folder");
    expect(l.engine_assets.empty(), "a package never falls back to the engine checkout's assets/");
    expect(l.project_root() == root, "the package itself is the project root");
    const auto roots = l.shader_roots(l.project_root());
    expect(roots.size() == 1 && roots[0] == (root / "assets" / "shaders").string(), "one merged shader directory");
    for (const auto& r : roots) {
        expect(r.find(ROOT_DIR) == std::string::npos && r.find(PROJ_DIR) == std::string::npos,
               "no packaged shader root points into the source tree (" + r + ")");
    }
    expect(l.manifest && l.manifest->name == "Game" && l.manifest->profile == "shipping", "the manifest round-trips");
}

COOPA_TEST(app_bundle_is_detected_by_its_resources_manifest) {
    namespace fs = std::filesystem;
    const fs::path app = coopa::test::scratch_dir("layout_app") / "Game.app" / "Contents";
    fs::create_directories(app / "MacOS");
    fs::create_directories(app / "Resources");
    write_text_file(app / "MacOS" / "Game", "");
    toy::core::PackageManifest m;
    m.name = "Game";
    m.bundle_id = "com.example.game";
    m.save(app / "Resources" / toy::core::PackageManifest::k_file_name);
    const auto l = toy::core::RuntimeLayout::detect(app / "MacOS" / "Game");
    expect(l.packaged() && l.resources_root == app / "Resources", "Contents/Resources/toy_package.yaml marks a .app");
#if defined(__APPLE__)
    expect(l.app_id() == "com.example.game", "a .app's user directories are named by its bundle id");
#endif
}

COOPA_TEST(source_run_uses_compiled_shaders_only) {
    namespace fs = std::filesystem;
    const fs::path dir = coopa::test::scratch_dir("layout_source");
    write_text_file(dir / "game", "");
    const auto l = toy::core::RuntimeLayout::detect(dir / "game");
    expect(!l.packaged(), "no marker: running from source");
    expect(l.engine_assets == fs::path(ROOT_DIR) / "assets", "from source the engine checkout is the fallback layer");
    const auto roots = l.shader_roots(fs::path(ROOT_DIR));
    const fs::path built = toy::core::RuntimeLayout::compiled_shader_dir();
    expect(!built.empty(), "the build compiles shaders into its own tree (TOY_SHADER_BUILD_DIR)");
    expect(roots.size() == 3 && roots[0] == (built / "toyengine_shaders").string() && roots[1] == (built / "shaders").string() &&
           roots[2] == (built / "uicoopa_shaders").string(), "from source: engine, gfxcoopa, uicoopa compiled shaders in -I order");
    for (const auto& r : roots) {
        expect(r.rfind((fs::path(ROOT_DIR) / "assets").string(), 0) != 0 && r.rfind(std::string(PROJ_DIR), 0) != 0,
               "no shader root is a source directory (" + r + ")");
        expect(fs::exists(fs::path(r)), "the compiled shader directory exists (" + r + ")");
    }
    expect(fs::exists(built / "toyengine_shaders" / "gbuffer.vert.spv"), "the engine's shaders are compiled there");
    // A project with shaders of its own: its compiled ones first, then its own build tree's.
    const fs::path proj = coopa::test::scratch_dir("layout_project");
    fs::create_directories(proj / "assets" / "shaders");
    const auto proots = l.shader_roots(proj);
    expect(proots.size() == 5 && proots[0] == (built / "project_shaders").string() &&
           proots[1] == (proj / "build" / "shaders" / "project_shaders").string(), "a project's own shaders come first");
}

COOPA_TEST(user_dirs_follow_home) {
    ScopedEnv home("HOME", "/tmp/toy_home_test");
#if defined(__APPLE__)
    expect(toy::core::user_data_dir("g") == std::filesystem::path("/tmp/toy_home_test/Library/Application Support/g"),
           "macOS user data under ~/Library/Application Support");
    expect(toy::core::user_log_dir("g") == std::filesystem::path("/tmp/toy_home_test/Library/Logs/g"), "macOS logs under ~/Library/Logs");
#else
    ScopedEnv xdg("XDG_DATA_HOME", "/tmp/toy_xdg");
    expect(toy::core::user_data_dir("g") == std::filesystem::path("/tmp/toy_xdg/g"), "Linux user data under $XDG_DATA_HOME");
#endif
    expect(toy::core::debug_env("PATH") != nullptr, "debug env hooks are live in a non-shipping build");
}
