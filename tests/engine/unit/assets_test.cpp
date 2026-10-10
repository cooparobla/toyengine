/**
 * @file assets_test.cpp
 * @brief The shipped assets/ tree as data: every YAML asset survives the .caml encode/decode
 *        round trip as an equal node (`caml_roundtrips_every_asset` -- the create-* skills run it by
 *        name to validate assets), and the shipped config and a shipped scene load identically from
 *        their packaged .caml form.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <coopa/scene/scene_loader.h>
#include <coopa/yaml/document.h>
#include <toyengine/core/caml_codec.h>
#include <toyengine/core/config.h>

#include "engine/support/checks.h"
#include "engine/support/scratch_files.h"

COOPA_TEST_SUITE("assets");

using namespace toy::test;

namespace {

/** @brief Every .yaml/.yml file under assets/, sorted so failures list in a stable order. */
std::vector<std::filesystem::path> asset_yaml_files() {
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::recursive_directory_iterator(std::string(ROOT_DIR) + "/assets")) {
        if (!e.is_regular_file()) continue;
        const std::string ext = e.path().extension().string();
        if (ext == ".yaml" || ext == ".yml") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

/** @brief Every YAML asset in the repo survives yaml -> .caml -> load_document as an equal node. */
COOPA_TEST(caml_roundtrips_every_asset) {
    toy::core::install_caml_codec();   // what the game's main() does before loading anything
    const std::filesystem::path dir = coopa::test::scratch_dir("caml_roundtrip");
    const auto files = asset_yaml_files();
    expect(files.size() > 10, "assets/ has YAML files to round-trip (found " + std::to_string(files.size()) + ")");
    int index = 0;
    for (const auto& src : files) {
        const std::filesystem::path out = dir / (std::to_string(index++) + ".caml");
        toy::core::encode_caml_file(src, out);
        const fkyaml::node plain   = coopa::yaml::load_document(src);
        const fkyaml::node decoded = coopa::yaml::load_document(out);
        expect(plain == decoded, "caml round-trip preserves " + src.lexically_relative(ROOT_DIR).string());
    }
}

/** @brief AppConfig and SceneLoader read packaged files identically to their YAML originals. */
COOPA_TEST(shipped_config_and_scene_load_identically_from_caml) {
    toy::core::install_caml_codec();   // what the game's main() does before loading anything
    const std::filesystem::path dir = coopa::test::scratch_dir("caml_config_scene");
    const std::string config_yaml = std::string(ROOT_DIR) + "/assets/config.yaml";
    toy::core::encode_caml_file(config_yaml, dir / "config.caml");

    const toy::core::AppConfig a = toy::core::AppConfig::load(config_yaml);
    const toy::core::AppConfig b = toy::core::AppConfig::load((dir / "config.yaml").string()); // finds config.caml
    expect(a.scene.default_scene == b.scene.default_scene, "config.caml: scene.default_scene matches");
    expect(a.window.width == b.window.width && a.window.height == b.window.height, "config.caml: window size matches");
    expect(a.render.render_width == b.render.render_width && a.render.aa_mode == b.render.aa_mode,
           "config.caml: render settings match");

    const std::filesystem::path scene_src = std::string(ROOT_DIR) + "/assets/scenes/tests/physics/physics_test";
    package_tree_as_caml(scene_src, dir / "physics_test");
    coopa::scene::Scene plain  = coopa::scene::SceneLoader::load((scene_src / "scene.yaml").string());
    coopa::scene::Scene packed = coopa::scene::SceneLoader::load((dir / "physics_test" / "scene.yaml").string());
    std::vector<std::string> plain_names, packed_names;
    for (const auto& o : plain.root_objects())  plain_names.push_back(o->name());
    for (const auto& o : packed.root_objects()) packed_names.push_back(o->name());
    expect(!plain_names.empty() && plain_names == packed_names,
           "scene.caml loads the same root objects (" + std::to_string(packed_names.size()) + ")");
}
