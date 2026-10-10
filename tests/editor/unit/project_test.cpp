/**
 * @file project_test.cpp
 * @brief Projects on disk: a .toy marks a project, create() lays down assets/ without touching an
 * existing .toy, a new config.yaml is the engine's with only the project's keys changed, and the
 * engine checkout stays a valid project.
 */

#include <coopa/testing/test.h>

#include "editor/app/project.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("project");

namespace toy::editor::testing {

COOPA_TEST(toy_files_create_and_the_starting_config) {
    // A toyhub-made project starts as a .toy and scripts; the editor creates assets/ on first open.
    use_scratch_home();
    const fs::path root = coopa::test::scratch_dir("Fancy Game");
    write_text(root / "fancy.toy",
               "format: toyproject\nversion: 1\nname: \"Ignored\"\ntarget: fancy_game\n"
               "engine:\n  source: git@github.com:cooparobla/toyengine.git\n  ref: abc123\n");
    Project p(root);
    expect(p.valid() && !p.has_assets(), "a .toy without assets/ is a valid project awaiting its skeleton");
    expect(p.name() == "Fancy Game", "a project's name is its folder's, whatever the .toy says");
    expect(Project(root.string() + "/").name() == "Fancy Game", "...with or without a trailing slash");
    expect(p.project_file() == root / "fancy.toy", "project_file() finds the .toy");
    const std::string before = [&] { std::ifstream in(root / "fancy.toy"); return std::string(std::istreambuf_iterator<char>(in), {}); }();
    p = Project::create(root);
    expect(p.has_assets() && coopa::yaml::document_exists(p.assets() / "scenes/main/scene.yaml"), "create() lays down assets/");
    const std::string after = [&] { std::ifstream in(root / "fancy.toy"); return std::string(std::istreambuf_iterator<char>(in), {}); }();
    expect(before == after, "create() leaves an existing .toy byte-identical");

    // config.yaml starts as the engine's own: the same settings, only the title and default scene
    // are the project's -- and a game doesn't save a screenshot on exit. Compared as documents, so
    // the engine's file may be in any layout (as written by hand, or as the editor saves it).
    Node engine_cfg = coopa::yaml::load_document(fs::path(ROOT_DIR) / "assets" / "config.yaml");
    const Node project_cfg = coopa::yaml::load_document(p.config_path());
    const bool project_values = project_cfg.contains("window") && get_string(project_cfg["window"], "title") == "Fancy Game" &&
                                project_cfg.contains("scene") &&
                                get_string(project_cfg["scene"], "default_scene") == "assets/scenes/main/scene.yaml" &&
                                project_cfg.contains("output") && project_cfg["output"].contains("save_on_exit") &&
                                project_cfg["output"]["save_on_exit"].is_boolean() &&
                                !project_cfg["output"]["save_on_exit"].get_value<bool>();
    engine_cfg["window"]["title"] = Node(std::string("Fancy Game"));
    engine_cfg["scene"]["default_scene"] = Node(std::string("assets/scenes/main/scene.yaml"));
    engine_cfg["output"]["save_on_exit"] = Node(false);
    expect(project_values && engine_cfg == project_cfg,
           "a new project's config.yaml is the engine's, with only the title, default scene and save_on_exit changed");
    int toys = 0;
    for (const auto& e : fs::directory_iterator(root)) toys += e.path().extension() == ".toy";
    expect(toys == 1, "create() adds no second .toy");

    // A bare folder gets a minimal .toy named after it.
    const fs::path bare = coopa::test::scratch_dir("bare_project");
    Project b = Project::create(bare);
    expect(b.project_file() == bare / "bare_project.toy", "create() writes <dir>.toy into a bare folder");
    expect(b.name() == "bare_project", "...whose name is the folder name");
    const Node toy = Project::load_toy(b.project_file());
    expect(get_string(toy, "format") == "toyproject" && !toy.contains("name"), "...in the toyproject format, with no name key");

    // An assets-only folder (this repository) is still a project, named after its folder.
    Project repo{fs::path(ROOT_DIR)};
    expect(repo.valid() && repo.name() == fs::path(ROOT_DIR).filename().string(), "the engine checkout stays a valid project");
}

} // namespace toy::editor::testing
