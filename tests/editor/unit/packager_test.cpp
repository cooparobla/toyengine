/**
 * @file packager_test.cpp
 * @brief Packaging layers, device-free: a game project ships toyengine's runtime files (compiled
 * shaders, fonts) but never its scenes or content, a project file shadows the engine's, and a
 * shipping stage merges the engine's, gfxcoopa's and uicoopa's compiled shaders (engine first).
 * 
 * Not here: rendering the packaged project (gpu package) or running a real build (gpu build).
 */

#include <coopa/testing/test.h>

#include <toyengine/core/runtime_paths.h>

#include "editor/app/project.h"
#include "editor/build/packager.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("packager");

namespace toy::editor::testing {

COOPA_TEST(engine_fallback_ships_runtime_files_only) {
    use_scratch_home();
    const fs::path root = coopa::test::scratch_dir("fallback_src");
    Project project = Project::create(root);
    const fs::path out = coopa::test::scratch_dir("fallback_out");
    PackageOptions opt;
    opt.out_dir = out;
    opt.engine_assets = fs::path(ROOT_DIR) / "assets";
    opt.library_layers = default_library_layers(project.root());   // the compiled shaders (build tree)
    const PackageReport rep = package_project(project, opt);
    expect(rep.ok(), "packaging with the engine fallback succeeds");
    bool spv = false, glsl = false;
    for (const auto& e : fs::directory_iterator(out / "assets" / "shaders")) {
        spv |= e.path().extension() == ".spv";
        glsl |= e.path().extension() == ".frag" || e.path().extension() == ".vert";
    }
    expect(spv && !glsl, "engine shaders ship compiled only");
    expect(fs::is_directory(out / "assets" / "fonts"), "engine fonts ship");
    expect(!fs::exists(out / "assets" / "scenes" / "demos" / "pixel_demo"), "engine scenes never ship");
    expect(coopa::yaml::document_exists(out / "assets" / "scenes" / "main" / "scene.yaml"), "the project's own scene ships");
    expect(!fs::exists(out / "assets" / "materials" / "brick.caml"), "engine content outside the runtime dirs never ships");

    // A project that ships its own copy of an engine runtime file keeps its own.
    write_text(project.assets() / "ui" / "marker.txt", "project");
    write_text(project.assets() / "fonts" / "LICENSE-OFL.txt", "project copy");
    const fs::path out2 = coopa::test::scratch_dir("fallback_out2");
    opt.out_dir = out2;
    expect(package_project(project, opt).ok(), "repackaging succeeds");
    std::ifstream lic(out2 / "assets" / "fonts" / "LICENSE-OFL.txt");
    std::string first;
    std::getline(lic, first);
    expect(first == "project copy", "a project file shadows the engine's of the same name");
}

COOPA_TEST(shipping_stages_compiled_shader_layers) {
    use_scratch_home();
    const fs::path root = coopa::test::scratch_dir("build_layers_src");
    Project project = Project::create(root);
    const fs::path out = coopa::test::scratch_dir("build_layers_out");
    PackageOptions opt;
    opt.out_dir = out;
    opt.encode_yaml = false;
    opt.shipping = true;
    opt.engine_assets = fs::path(ROOT_DIR) / "assets";
    opt.library_layers = default_library_layers(project.root());
    const PackageReport rep = package_project(project, opt);
    expect(rep.ok(), "staging succeeds");
    const fs::path sh = out / "assets" / "shaders";
    // Compiled shaders come from the build tree (toyengine/core/runtime_paths.h), never the checkout.
    const fs::path built = toy::core::RuntimeLayout::compiled_shader_dir();
    const fs::path engine_spv = built / "toyengine_shaders", gfx_spv = built / "shaders", ui_spv = built / "uicoopa_shaders";
    expect(fs::exists(sh / "gbuffer.vert.spv"), "the engine's compiled shaders are in the package");
    // A gfxcoopa shader (one the engine doesn't also ship) and a uicoopa UI shader, both merged in.
    bool gfx_only = false;
    for (const auto& e : fs::directory_iterator(gfx_spv)) {
        if (e.path().extension() != ".spv") continue;
        if (fs::exists(engine_spv / e.path().filename())) continue;
        gfx_only = fs::exists(sh / e.path().filename());
        break;
    }
    expect(gfx_only, "gfxcoopa's base shaders are merged into the package");
    bool ui = false;
    for (const auto& e : fs::directory_iterator(ui_spv)) {
        if (e.path().extension() == ".spv") { ui = fs::exists(sh / e.path().filename()); break; }
    }
    expect(ui, "uicoopa's UI shaders are merged into the package");
    bool glsl = false;
    for (const auto& e : fs::directory_iterator(sh)) glsl |= e.path().extension() != ".spv";
    expect(!glsl, "only compiled shaders ship (no GLSL, no depfiles)");
    // Should the engine and gfxcoopa ever ship a same-named shader, the package must carry the
    // engine's. The two directories normally share no names, so this usually only prints the note.
    bool precedence_checked = false;
    for (const auto& e : fs::directory_iterator(engine_spv)) {
        if (e.path().extension() != ".spv") continue;
        const fs::path gfx = gfx_spv / e.path().filename();
        if (!fs::exists(gfx) || fs::file_size(gfx) == fs::file_size(e.path())) continue;
        expect(fs::file_size(sh / e.path().filename()) == fs::file_size(e.path()),
               "the engine's " + e.path().filename().string() + " wins over gfxcoopa's (first match, like the runtime)");
        precedence_checked = true;
        break;
    }
    if (!precedence_checked) std::cout << "  (no differing engine/gfxcoopa shader pair to check precedence on)\n";
    expect(fs::exists(out / "assets" / "sounds" / "sounds.yaml"), "uicoopa's default UI sounds ship");
    const Node cfg = coopa::yaml::load_document(out / "assets" / "config.yaml");
    expect(cfg.contains("output") && !cfg["output"]["save_on_exit"].get_value<bool>(), "a shipping config never saves a screenshot on exit");
}

} // namespace toy::editor::testing
