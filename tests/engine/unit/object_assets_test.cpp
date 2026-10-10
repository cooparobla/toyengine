/**
 * @file object_assets_test.cpp
 * @brief Object assets (prefabs): a scene's `prefab: objects/x` instance -- its own name and
 *        Transform, child removal -- and SceneLoader::spawn() at runtime. UI prefabs' RectTransform
 *        rule lives in game_ui.
 */

#include <coopa/testing/test.h>

#include <filesystem>
#include <fstream>

#include <coopa/scene/scene_loader.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("object_assets");

/** @brief Object assets: `prefab: objects/x` from a scene, Transform replace, overrides, spawn. */
COOPA_TEST(prefab_instance_overrides_and_runtime_spawn) {
    namespace fs = std::filesystem;
    using coopa::scene::SceneLoader;
    const fs::path root = coopa::test::scratch_dir() / "prefab_project" / "assets";
    fs::remove_all(root.parent_path());
    fs::create_directories(root / "objects");
    fs::create_directories(root / "scenes" / "level");
    {
        std::ofstream o(root / "objects" / "crate.yaml");
        o << "format: toyengine-object\n"
             "object:\n"
             "  name: Crate\n"
             "  components:\n"
             "    - { type: Transform, position: { x: 5, y: 5, z: 5 }, scale: { x: 2, y: 2, z: 2 } }\n"
             "  children:\n"
             "    - name: Lid\n"
             "      components: [ { type: Transform, position: { x: 0, y: 0, z: 1 } } ]\n"
             "    - name: Handle\n"
             "      components: [ { type: Transform } ]\n";
    }
    {
        std::ofstream o(root / "scenes" / "level" / "scene.yaml");
        o << "format: toyengine\n"
             "scene:\n"
             "  scene_name: Level\n"
             "  root_objects:\n"
             "    - name: Crate.001\n"
             "      prefab: objects/crate\n"
             "      components: [ { type: Transform, position: { x: 1, y: 0, z: 0 } } ]\n"
             "      children: [ { name: Handle, remove: true } ]\n";
    }
    SceneLoader::set_search_roots({root.string()});
    coopa::scene::Scene scene = SceneLoader::load((root / "scenes" / "level" / "scene.yaml").string());
    expect(scene.root_objects().size() == 1, "the instance loads");
    if (scene.root_objects().empty()) return;
    auto* crate = scene.root_objects()[0].get();
    expect(crate->name() == "Crate.001", "the instance keeps its own name");
    const glm::vec3 p = crate->get_transform()->transform().position();
    const glm::vec3 sc = crate->get_transform()->transform().scale();
    expect(glm::distance(p, glm::vec3(1, 0, 0)) < 1e-5f && glm::distance(sc, glm::vec3(1)) < 1e-5f,
           "the instance's Transform replaces the prefab's root Transform");
    expect(crate->children().size() == 1 && crate->children()[0]->name() == "Lid", "children come from the prefab; remove: true drops one");

    auto* spawned = SceneLoader::spawn(scene, "objects/crate");
    expect(spawned && scene.root_objects().size() == 2 && spawned->children().size() == 2, "spawn() instantiates an object asset at runtime");
    bool threw = false;
    try { SceneLoader::spawn(scene, "objects/missing"); } catch (const std::exception&) { threw = true; }
    expect(threw, "spawning a missing asset throws");
    SceneLoader::set_search_roots({});
}
