/**
 * @file asset_refs_test.cpp
 * @brief Asset references and tags, device-free: renaming an asset rewrites every reference to it (and
 * moves LOD sidecars), tags are the folders between an asset's type folder and the asset, the
 * engine's AssetIndex finds an asset by name whatever its tags, and re-tagging rewrites only
 * full-path references.
 * 
 * Not here: the Asset panel's tag filter and Set Tags (gpu asset_browser).
 */

#include <coopa/testing/test.h>

#include <coopa/asset/asset_index.h>

#include "editor/app/project.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("asset_refs");

namespace toy::editor::testing {

COOPA_TEST(renaming_an_asset_rewrites_every_reference) {
    expect(mesh_ref("meshes/rock.yaml") == "rock", "mesh_ref: top-level mesh");
    expect(mesh_ref("meshes/props/rock.yaml") == "rock", "mesh_ref drops tag folders (meshes are found by name)");
    expect(mesh_ref("scenes/lake/meshes/basin.yaml") == "basin", "mesh_ref: scene-local mesh");
    expect(strip_yaml_ext("objects/props/crate.yaml") == "objects/props/crate", "strip_yaml_ext");

    use_scratch_home();
    const fs::path root = coopa::test::scratch_dir("rename_refs");
    const fs::path a = root / "assets";
    auto write = [&](const std::string& rel, const std::string& text) {
        fs::create_directories((a / rel).parent_path());
        std::ofstream(a / rel) << text;
    };
    write("objects/props/crate.yaml", "object:\n  name: crate\n  components: []\n");
    write("meshes/props/rock.yaml", "vertices: []\nfaces: []\n");
    write("meshes/props/rock.lod.yaml", "lods: []\n");
    write("materials/stone.yaml", "albedo: {r: 1.0, g: 1.0, b: 1.0}\ntexture_albedo: textures/stone.png\n");
    write("textures/stone.png", "png");
    write("scenes/lake/meshes/basin.yaml", "vertices: []\nfaces: []\n");
    write("scenes/lake/scene.yaml",
          "scene:\n  scene_name: lake\n  root_objects:\n"
          "    - name: props/rock\n      prefab: objects/props/crate\n      components:\n"
          "        - type: MeshRenderer\n          mesh_path: props/rock\n          material: materials/stone\n"
          "        - type: MeshCollider\n          mesh_path: basin\n");
    write("scenes/other/scene.yaml",
          "scene:\n  scene_name: other\n  root_objects:\n"
          "    - name: basin\n      components:\n        - type: MeshRenderer\n          mesh_path: basin\n");
    Project project(root);

    auto scene = [&](const std::string& s) { return coopa::yaml::load_document(a / "scenes" / s / "scene.yaml").at("scene").at("root_objects")[0]; };
    auto rewritten = project.rename_asset("objects/props/crate.yaml", "objects/props/box.yaml");
    expect(get_string(scene("lake"), "prefab") == "objects/props/box", "renaming an object asset rewrites prefab:");
    expect(rewritten.size() == 1, "only the referring scene is rewritten");

    project.rename_asset("meshes/props/rock.yaml", "meshes/props/boulder.yaml");
    const Node lake = scene("lake");
    expect(get_string(lake.at("components")[0], "mesh_path") == "props/boulder", "renaming a mesh rewrites mesh_path");
    expect(get_string(lake, "name") == "props/rock", "an object name equal to the old ref is left alone");
    expect(fs::exists(a / "meshes/props/boulder.lod.yaml") && !fs::exists(a / "meshes/props/rock.lod.yaml"), "the LOD sidecar moves too");

    project.rename_asset("materials/stone.yaml", "materials/granite.yaml");
    expect(get_string(scene("lake").at("components")[0], "material") == "materials/granite", "renaming a material rewrites material:");

    project.rename_asset("textures/stone.png", "textures/granite.png");
    expect(get_string(coopa::yaml::load_document(a / "materials/granite.yaml"), "texture_albedo") == "textures/granite.png",
           "renaming a texture rewrites the material's map");

    project.rename_asset("scenes/lake/meshes/basin.yaml", "scenes/lake/meshes/pool.yaml");
    expect(get_string(scene("lake").at("components")[1], "mesh_path") == "pool", "a scene-local mesh is renamed in its scene");
    expect(get_string(scene("other").at("components")[0], "mesh_path") == "basin", "other scenes' same-named meshes are untouched");
}

/**
 * @brief Tags: the folders between an asset's type folder and the asset. References name the
 *        type and the asset only, the engine finds it by name (coopa::asset::AssetIndex), and
 *        re-tagging moves the file (a scene: its folder), rewriting only full-path references.
 */
COOPA_TEST(tags_are_folders_and_retagging_rewrites_only_full_paths) {
    expect(asset_tags("materials/metal/brick.yaml") == std::vector<std::string>{"metal"}, "a material's tag is its folder");
    expect(asset_tags("scenes/tests/water/lake/scene.yaml") == std::vector<std::string>{"tests", "water"},
           "a scene's own folder is the asset, not a tag");
    expect(asset_tags("ui/themes/dark.yaml").empty() && asset_type_dir("ui/themes/dark.yaml") == "ui/themes",
           "ui/themes is a type folder, not a tag");
    expect(asset_tags("scenes/lake/meshes/basin.yaml").empty(), "scene-local files have no tags");
    expect(with_asset_tags("materials/brick.yaml", {"stone", "wall"}) == "materials/stone/wall/brick.yaml", "tags nest in order");
    expect(with_asset_tags("scenes/tests/lake/scene.yaml", {}) == "scenes/lake/scene.yaml", "untagging a scene moves its folder up");
    expect(short_ref("materials/metal/brick.yaml") == "materials/brick.yaml" &&
           short_ref("scenes/tests/lake/scene.yaml") == "scenes/lake/scene.yaml" &&
           short_ref("ui/themes/dark.yaml") == "ui/themes/dark.yaml", "short refs drop tag folders");

    use_scratch_home();
    const fs::path root = coopa::test::scratch_dir("asset_tags");
    const fs::path a = root / "assets";
    auto write = [&](const std::string& rel, const std::string& text) {
        fs::create_directories((a / rel).parent_path());
        std::ofstream(a / rel) << text;
    };
    write("config.yaml", "scene:\n  default_scene: \"assets/scenes/lake/scene.yaml\"\n");
    write("materials/metal/brick.yaml", "albedo: {r: 1.0, g: 0.0, b: 0.0}\n");
    write("materials/stone/brick_old.yaml", "albedo: {r: 0.0, g: 1.0, b: 0.0}\n");
    write("meshes/props/rock.yaml", "vertices: []\nfaces: []\n");
    write("scenes/lake/meshes/basin.yaml", "vertices: []\nfaces: []\n");
    write("scenes/lake/scene.yaml",
          "scene:\n  scene_name: lake\n  root_objects:\n"
          "    - name: a\n      components:\n"
          "        - type: MeshRenderer\n          mesh_path: rock\n          material: materials/brick\n"
          "        - type: MeshRenderer\n          mesh_path: props/rock\n          material: materials/metal/brick\n");
    Project project(root);

    using coopa::asset::AssetIndex;
    expect(AssetIndex::find(a, "materials/brick.yaml") == a / "materials/metal/brick.yaml", "the index finds a tagged asset by name");
    expect(AssetIndex::find(a, "meshes/rock.yaml") == a / "meshes/props/rock.yaml", "...meshes too");
    expect(AssetIndex::find(a, "scenes/lake/scene.yaml") == a / "scenes/lake/scene.yaml", "...and scenes by their folder");
    expect(!AssetIndex::find(a, "materials/missing.yaml") && !AssetIndex::find(a, "objects/brick.yaml"),
           "names are per type: a material is no object");
    expect(project.absolute("materials/brick.yaml") == a / "materials/metal/brick.yaml", "Project::absolute finds it by name");

    auto comps = [&](const std::string& scene_rel) {
        return coopa::yaml::load_document(a / scene_rel).at("scene").at("root_objects")[0].at("components");
    };
    // Re-tag a material: short references stay, the full-path one follows.
    project.rename_asset("materials/metal/brick.yaml", "materials/wall/red/brick.yaml");
    expect(fs::exists(a / "materials/wall/red/brick.yaml") && !fs::exists(a / "materials/metal/brick.yaml"), "re-tagging moves the file");
    expect(get_string(comps("scenes/lake/scene.yaml")[0], "material") == "materials/brick", "a short reference is left alone");
    expect(get_string(comps("scenes/lake/scene.yaml")[1], "material") == "materials/wall/red/brick", "a full-path reference follows");
    expect(AssetIndex::find(a, "materials/brick.yaml") == a / "materials/wall/red/brick.yaml", "the index sees the move");

    // Re-tag a mesh: a bare name stays, an old subfolder ref follows.
    project.rename_asset("meshes/props/rock.yaml", "meshes/nature/rock.yaml");
    expect(get_string(comps("scenes/lake/scene.yaml")[0], "mesh_path") == "rock" &&
           get_string(comps("scenes/lake/scene.yaml")[1], "mesh_path") == "nature/rock", "mesh refs: name kept, path followed");

    // Re-tag a scene: the whole folder moves (scene-local meshes along) and config follows.
    project.rename_asset("scenes/lake/scene.yaml", "scenes/tests/water/lake/scene.yaml");
    expect(fs::exists(a / "scenes/tests/water/lake/scene.yaml") && fs::exists(a / "scenes/tests/water/lake/meshes/basin.yaml") &&
           !fs::exists(a / "scenes/lake"), "a scene moves as its folder");
    const std::string cfg = get_string(coopa::yaml::load_document(a / "config.yaml").at("scene"), "default_scene");
    expect(cfg == "assets/scenes/lake/scene.yaml", "config's default_scene (the scene's short path) is left alone");
    expect(project.scenes() == std::vector<std::string>{"scenes/tests/water/lake/scene.yaml"}, "the moved scene is listed");
    expect(AssetIndex::find(a, "scenes/lake/scene.yaml") == a / "scenes/tests/water/lake/scene.yaml",
           "the scene's old short path still finds it");

}

} // namespace toy::editor::testing
