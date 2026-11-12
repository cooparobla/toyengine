/**
 * @file package_test.cpp
 * @brief File > Package: every YAML file is encoded to .caml and the packaged project renders
 * identically to its source.
 */

#include <coopa/testing/test.h>

#include <gfxcoopa/util/image_readback.h>

#include "editor/build/packager.h"
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("package");

namespace toy::editor::testing {

/** @brief File > Package to .caml: every YAML file is encoded, none is left plain, and the
 *         packaged project renders exactly like its source (FIXED_DT=0, accumulating effects off). */
COOPA_TEST(the_packaged_project_renders_identically) {
    setenv("FIXED_DT", "0", 1);
    setenv("NO_INPUT", "1", 1);
    install_codecs_once();   // the packaged project loads through the .caml decoder
    Project project = new_project("package_src");
    const fs::path root = project.root();
    const fs::path out = coopa::test::scratch_dir("package_out");
    PackageOptions opt;
    opt.out_dir = out;
    const PackageReport rep = package_project(project, opt);
    expect(rep.ok() && rep.encoded >= 6, "packaging encodes every YAML file (" + std::to_string(rep.encoded) + ")");
    bool any_yaml = false;
    for (const auto& e : fs::recursive_directory_iterator(out)) any_yaml |= e.path().extension() == ".yaml";
    expect(!any_yaml, "no plain YAML is left in the package");

    auto render = [&](const fs::path& proj) {
        toy::core::AppConfig cfg = toy::core::AppConfig::load((proj / "assets/config.yaml").string());
        cfg.window.visible = false;
        cfg.window.width = 640;
        cfg.window.height = 360;
        cfg.render.ssao_temporal_enabled = false;
        cfg.render.ssr_temporal_enabled = false;
        // Likewise every other effect that accumulates across frames (a new project's config.yaml
        // is the engine's, which turns them on): this compares YAML vs .caml loading, not timing.
        cfg.render.aa_mode = "off";
        cfg.render.auto_exposure_enabled = false;
        cfg.output.save_on_exit = false;
        toy::core::EngineOptions o;
        o.project_root = proj;
        toy::core::Engine e(cfg, o);
        tick(e, 8);
        return e.capture_image(true);
    };
    const auto a = render(root);
    const auto b = render(out);
    size_t diff = 0;
    for (size_t i = 0; i < std::min(a.pixels.size(), b.pixels.size()); i += 4) diff += a.pixels[i] != b.pixels[i];
    expect(a.pixels.size() == b.pixels.size() && diff == 0, "the packaged (.caml) project renders identically (" + std::to_string(diff) + " px)");
}

} // namespace toy::editor::testing
