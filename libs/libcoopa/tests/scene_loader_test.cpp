/**
 * @file scene_loader_test.cpp
 * @brief coopa::scene::SceneLoader: `inherit_from` resolution (object- and scene-level,
 *        component merge by type and by id, child merge/append, removal, chains, multiple
 *        bases, cycles and missing files, asset paths resolved against the declaring file) and
 *        the async read + incremental Builder path.
 *
 * Fixtures are real files in the test's scratch dir so relative paths behave exactly like they
 * do on disk. Merges are verified against attached component instances (TestTag), since
 * SceneLoader never exposes the merged YAML back out.
 */
#include <coopa/testing/test.h>

#include "support/scratch_file.h"

#include <coopa/job/engine.h>
#include <coopa/scene/component.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_inherit.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <coopa/yaml/document.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

COOPA_TEST_SUITE("scene_loader");

using libcoopa_test::write_scratch_file;

namespace {

// A minimal test-only component so merges can be verified against real
// attached component instances rather than by re-inspecting YAML.
class TestTagComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "TestTag"; }
    void start() override { started = true; }
    std::string id;
    std::string label;
    std::string extra;
    bool started = false;
};

// Proves ctx.resolve() finds a relative path against the file that actually
// declared it -- the provenance a prefab merged in from another directory
// needs to keep working.
class TestAssetComponent : public coopa::scene::Component {
public:
    std::string type_name() const override { return "TestAsset"; }
    std::string resolved_path;
};

/**
 * @brief Registers the TestTag/TestAsset parsers. Called by every test rather than once: the
 *        registry is process-global and another suite in the same process (animator's scene
 *        YAML test) clears it, and re-registering a name just replaces the parser.
 */
void register_test_components() {
    using coopa::scene::SceneLoader;
    SceneLoader::register_component_parser(
        "TestTag",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* c = obj.add_component<TestTagComponent>();
            if (node.contains("id"))    c->id    = node.at("id").get_value<std::string>();
            if (node.contains("label")) c->label = node.at("label").get_value<std::string>();
            if (node.contains("extra")) c->extra = node.at("extra").get_value<std::string>();
        });
    SceneLoader::register_component_parser(
        "TestAsset",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext& ctx) {
            auto* c = obj.add_component<TestAssetComponent>();
            if (node.contains("file")) c->resolved_path = ctx.resolve(node.at("file").get_value<std::string>());
        });
}

std::vector<TestTagComponent*> find_tags(const coopa::scene::SceneObject& obj) {
    std::vector<TestTagComponent*> result;
    for (const auto& c : obj.components()) {
        if (auto* t = dynamic_cast<TestTagComponent*>(c.get())) result.push_back(t);
    }
    return result;
}

TestTagComponent* find_tag_by_id(const coopa::scene::SceneObject& obj, const std::string& id) {
    for (auto* t : find_tags(obj)) {
        if (t->id == id) return t;
    }
    return nullptr;
}

/** @brief The single TestTag's label on `obj` ("" when there is not exactly one). */
std::string only_label(const coopa::scene::SceneObject& obj) {
    auto tags = find_tags(obj);
    return tags.size() == 1 ? tags[0]->label : std::string();
}

coopa::scene::Scene load(const std::string& path) {
    register_test_components();
    return coopa::scene::SceneLoader::load(path);
}

// The `states` merge is asserted on the merged YAML rather than on an attached Animator:
// coopa::anim is a layer above this one, and SceneInheritance::resolve is the public entry
// point that does expose the merged node (SceneLoader itself does not).
fkyaml::node merged_components(const std::string& scene_path, const std::string& object_name) {
    const fkyaml::node root = coopa::scene::SceneInheritance::resolve(
        scene_path, [](const std::string& p) { return coopa::yaml::load_document(p); });
    for (const auto& obj : root.at("scene").at("root_objects")) {
        if (obj.contains("name") && obj.at("name").get_value<std::string>() == object_name) {
            return obj.contains("components") ? obj.at("components") : fkyaml::node::sequence();
        }
    }
    return fkyaml::node::sequence();
}

/** @brief The named component's named list field, or an empty sequence. */
fkyaml::node merged_list(const std::string& scene_path, const std::string& type, const std::string& field) {
    for (const auto& c : merged_components(scene_path, "Derived")) {
        if (!c.contains("type") || c.at("type").get_value<std::string>() != type) continue;
        return c.contains(field) ? c.at(field) : fkyaml::node::sequence();
    }
    return fkyaml::node::sequence();
}

/** @brief `field` of the entry named `name`, or "" when absent. */
std::string entry_field(const fkyaml::node& list, const std::string& name, const std::string& field) {
    for (const auto& e : list) {
        if (!e.contains("name") || e.at("name").get_value<std::string>() != name) continue;
        return e.contains(field) ? e.at(field).get_value<std::string>() : std::string();
    }
    return {};
}

/** @brief Numeric `field` of the entry named `name`, or NaN when absent. */
double entry_number(const fkyaml::node& list, const std::string& name, const std::string& field) {
    for (const auto& e : list) {
        if (!e.contains("name") || e.at("name").get_value<std::string>() != name) continue;
        if (e.contains(field)) return e.at(field).get_value<double>();
    }
    return std::nan("");
}

std::vector<std::string> entry_names(const fkyaml::node& list) {
    std::vector<std::string> out;
    for (const auto& e : list) {
        out.push_back(e.contains("name") ? e.at("name").get_value<std::string>() : std::string());
    }
    return out;
}

/** @brief A base rig owning three clips, plus a derived object overriding `states` with `over`. */
std::string write_rig_fixture(const std::string& over) {
    write_scratch_file("rig.yaml",
        "object:\n"
        "  name: Rig\n"
        "  components:\n"
        "    - type: Animator\n"
        "      auto_play: idle\n"
        "      default_crossfade: 0.15\n"
        "      states:\n"
        "        - {name: idle, clip: animations/rig/idle.yaml}\n"
        "        - {name: walk, clip: animations/rig/walk.yaml}\n"
        "        - {name: run, clip: animations/rig/run.yaml}\n");
    return write_scratch_file("scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Derived\n"
        "      inherit_from: rig.yaml\n"
        "      components:\n"
        "        - type: Animator\n"
        "          states:\n" + over);
}

} // namespace

COOPA_TEST(object_inherits_components_and_children_from_a_prefab_file) {
    write_scratch_file("prefab.yaml",
        "object:\n"
        "  name: Base\n"
        "  components:\n"
        "    - type: Transform\n"
        "      position: { x: 1.0, y: 2.0, z: 3.0 }\n"
        "    - type: TestTag\n"
        "      label: base-label\n"
        "  children:\n"
        "    - name: Child1\n"
        "      components:\n"
        "        - type: TestTag\n"
        "          label: child-label\n");
    const std::string scene_path = write_scratch_file("scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Derived\n"
        "      inherit_from: prefab.yaml\n"
        "      components:\n"
        "        - type: TestTag\n"
        "          label: override-label\n");

    coopa::scene::Scene scene = load(scene_path);
    coopa::scene::SceneObject* obj = scene.find_object("Derived");
    ASSERT_TRUE(obj != nullptr);
    ASSERT_TRUE(obj->get_transform() != nullptr);
    EXPECT_NEAR(obj->get_transform()->transform().position().x, 1.0f, 1e-5f);
    EXPECT_EQ(only_label(*obj), std::string("override-label"));

    coopa::scene::SceneObject* child = obj->find_descendant("Child1");
    ASSERT_TRUE(child != nullptr);
    EXPECT_EQ(only_label(*child), std::string("child-label"));
}

COOPA_TEST(component_override_merges_field_by_field_by_type) {
    write_scratch_file("prefab.yaml",
        "object:\n"
        "  name: Base\n"
        "  components:\n"
        "    - type: TestTag\n"
        "      label: base-label\n"
        "      extra: base-extra\n");
    const std::string scene_path = write_scratch_file("scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Derived\n"
        "      inherit_from: prefab.yaml\n"
        "      components:\n"
        "        - type: TestTag\n"
        "          label: new-label\n");

    coopa::scene::Scene scene = load(scene_path);
    auto tags = find_tags(*scene.find_object("Derived"));
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0]->label, std::string("new-label"));
    EXPECT_EQ(tags[0]->extra, std::string("base-extra"));
}

COOPA_TEST(component_id_selects_which_same_type_component_to_override) {
    write_scratch_file("prefab.yaml",
        "object:\n"
        "  name: Base\n"
        "  components:\n"
        "    - type: TestTag\n"
        "      id: A\n"
        "      label: A-base\n"
        "    - type: TestTag\n"
        "      id: B\n"
        "      label: B-base\n");
    const std::string scene_path = write_scratch_file("scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Derived\n"
        "      inherit_from: prefab.yaml\n"
        "      components:\n"
        "        - type: TestTag\n"
        "          id: B\n"
        "          label: B-override\n");

    coopa::scene::Scene scene = load(scene_path);
    coopa::scene::SceneObject* obj = scene.find_object("Derived");
    ASSERT_EQ(find_tags(*obj).size(), 2u);
    ASSERT_TRUE(find_tag_by_id(*obj, "A") && find_tag_by_id(*obj, "B"));
    EXPECT_EQ(find_tag_by_id(*obj, "A")->label, std::string("A-base"));
    EXPECT_EQ(find_tag_by_id(*obj, "B")->label, std::string("B-override"));
}

COOPA_TEST(children_merge_by_name_and_new_ones_append_in_order) {
    write_scratch_file("prefab.yaml",
        "object:\n"
        "  name: Base\n"
        "  children:\n"
        "    - name: A\n"
        "      components: [ { type: TestTag, label: A-base } ]\n"
        "    - name: B\n"
        "      components: [ { type: TestTag, label: B-base } ]\n");
    const std::string scene_path = write_scratch_file("scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Derived\n"
        "      inherit_from: prefab.yaml\n"
        "      children:\n"
        "        - name: B\n"
        "          components: [ { type: TestTag, label: B-override } ]\n"
        "        - name: C\n"
        "          components: [ { type: TestTag, label: C-new } ]\n");

    coopa::scene::Scene scene = load(scene_path);
    coopa::scene::SceneObject* obj = scene.find_object("Derived");
    ASSERT_EQ(obj->children().size(), 3u);
    EXPECT_EQ(obj->children()[0]->name(), std::string("A"));
    EXPECT_EQ(obj->children()[1]->name(), std::string("B"));
    EXPECT_EQ(obj->children()[2]->name(), std::string("C"));
    EXPECT_EQ(only_label(*obj->children()[0]), std::string("A-base"));
    EXPECT_EQ(only_label(*obj->children()[1]), std::string("B-override"));
    EXPECT_EQ(only_label(*obj->children()[2]), std::string("C-new"));
}

COOPA_TEST(remove_true_drops_inherited_components_and_children) {
    write_scratch_file("prefab.yaml",
        "object:\n"
        "  name: Base\n"
        "  components:\n"
        "    - type: TestTag\n"
        "      label: keep\n"
        "    - type: TestTag\n"
        "      id: doomed\n"
        "      label: remove-me\n"
        "  children:\n"
        "    - name: KeepChild\n"
        "    - name: RemoveChild\n");
    const std::string scene_path = write_scratch_file("scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Derived\n"
        "      inherit_from: prefab.yaml\n"
        "      components:\n"
        "        - type: TestTag\n"
        "          id: doomed\n"
        "          remove: true\n"
        "      children:\n"
        "        - name: RemoveChild\n"
        "          remove: true\n");

    coopa::scene::Scene scene = load(scene_path);
    coopa::scene::SceneObject* obj = scene.find_object("Derived");
    EXPECT_EQ(only_label(*obj), std::string("keep"));
    ASSERT_EQ(obj->children().size(), 1u);
    EXPECT_EQ(obj->children()[0]->name(), std::string("KeepChild"));
}

COOPA_TEST(animator_states_merge_by_name_keeping_the_rest_of_the_set) {
    const std::string scene = write_rig_fixture(
        "            - {name: walk, clip: animations/derived/strut.yaml}\n");

    const fkyaml::node states = merged_list(scene, "Animator", "states");
    // The override names one state: the other two still come from the base, in base order.
    ASSERT_EQ(states.size(), 3u);
    EXPECT_EQ(entry_names(states), (std::vector<std::string>{"idle", "walk", "run"}));
    EXPECT_EQ(entry_field(states, "walk", "clip"), std::string("animations/derived/strut.yaml"));
    EXPECT_EQ(entry_field(states, "idle", "clip"), std::string("animations/rig/idle.yaml"));
    EXPECT_EQ(entry_field(states, "run", "clip"), std::string("animations/rig/run.yaml"));
}

COOPA_TEST(animator_state_override_merges_key_by_key) {
    const std::string scene = write_rig_fixture("            - {name: run, speed: 1.5}\n");

    const fkyaml::node states = merged_list(scene, "Animator", "states");
    ASSERT_EQ(states.size(), 3u);
    // `speed` arrives without disturbing the clip the base pointed `run` at.
    EXPECT_EQ(entry_field(states, "run", "clip"), std::string("animations/rig/run.yaml"));
    EXPECT_NEAR(entry_number(states, "run", "speed"), 1.5, 1e-9);
    // Scalars beside `states` on the same component keep inheriting.
    bool checked = false;
    for (const auto& c : merged_components(scene, "Derived")) {
        if (!c.contains("type") || c.at("type").get_value<std::string>() != "Animator") continue;
        EXPECT_EQ(c.at("auto_play").get_value<std::string>(), std::string("idle"));
        checked = true;
    }
    EXPECT_TRUE(checked);
}

COOPA_TEST(animator_states_append_when_the_name_is_new) {
    const std::string scene = write_rig_fixture(
        "            - {name: swim, clip: animations/derived/swim.yaml}\n");

    const fkyaml::node states = merged_list(scene, "Animator", "states");
    ASSERT_EQ(states.size(), 4u);
    EXPECT_EQ(entry_names(states), (std::vector<std::string>{"idle", "walk", "run", "swim"}));
    EXPECT_EQ(entry_field(states, "swim", "clip"), std::string("animations/derived/swim.yaml"));
}

COOPA_TEST(animator_state_remove_true_drops_an_inherited_state) {
    const std::string scene = write_rig_fixture("            - {name: walk, remove: true}\n");

    const fkyaml::node states = merged_list(scene, "Animator", "states");
    ASSERT_EQ(states.size(), 2u);
    EXPECT_EQ(entry_names(states), (std::vector<std::string>{"idle", "run"}));
    // `remove` is bookkeeping and must not survive into the merged document.
    for (const auto& e : states) EXPECT_TRUE(!e.contains("remove"));
}

COOPA_TEST(animator_states_merge_into_the_first_of_a_duplicated_name) {
    write_scratch_file("dup_rig.yaml",
        "object:\n"
        "  name: Rig\n"
        "  components:\n"
        "    - type: Animator\n"
        "      states:\n"
        "        - {name: walk, clip: animations/rig/walk_a.yaml}\n"
        "        - {name: walk, clip: animations/rig/walk_b.yaml}\n");
    const std::string scene = write_scratch_file("scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Derived\n"
        "      inherit_from: dup_rig.yaml\n"
        "      components:\n"
        "        - type: Animator\n"
        "          states:\n"
        "            - {name: walk, clip: animations/derived/walk.yaml}\n");

    // A duplicated name is a malformed Animator; the merge warns and takes the first, which is
    // the one Animator::find_state() would play. It must not add a third.
    const fkyaml::node states = merged_list(scene, "Animator", "states");
    ASSERT_EQ(states.size(), 2u);
    EXPECT_EQ(states[0].at("clip").get_value<std::string>(), std::string("animations/derived/walk.yaml"));
    EXPECT_EQ(states[1].at("clip").get_value<std::string>(), std::string("animations/rig/walk_b.yaml"));
}

COOPA_TEST(lists_on_other_components_still_replace_wholesale) {
    // The states merge is deliberately Animator-only: every other list field keeps the
    // documented "sequences replace wholesale" behaviour.
    write_scratch_file("loot_base.yaml",
        "object:\n"
        "  name: Base\n"
        "  components:\n"
        "    - type: LootTable\n"
        "      entries:\n"
        "        - {name: coin, chance: 0.5}\n"
        "        - {name: gem, chance: 0.1}\n");
    const std::string scene = write_scratch_file("scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Derived\n"
        "      inherit_from: loot_base.yaml\n"
        "      components:\n"
        "        - type: LootTable\n"
        "          entries:\n"
        "            - {name: gem, chance: 0.9}\n");

    const fkyaml::node entries = merged_list(scene, "LootTable", "entries");
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entry_names(entries), (std::vector<std::string>{"gem"}));
}

COOPA_TEST(scene_level_inherit_merges_root_objects_and_settings) {
    write_scratch_file("base_scene.yaml",
        "scene:\n"
        "  scene_name: BaseScene\n"
        "  root_objects:\n"
        "    - name: Canvas\n"
        "      components: [ { type: TestTag, label: canvas-base } ]\n"
        "    - name: Extra\n");
    const std::string scene_path = write_scratch_file("derived_scene.yaml",
        "scene:\n"
        "  inherit_from: base_scene.yaml\n"
        "  scene_name: DerivedScene\n"
        "  root_objects:\n"
        "    - name: Canvas\n"
        "      components: [ { type: TestTag, label: canvas-override } ]\n"
        "    - name: New\n");

    coopa::scene::Scene scene = load(scene_path);
    EXPECT_EQ(scene.name(), std::string("DerivedScene"));
    EXPECT_EQ(only_label(*scene.find_object("Canvas")), std::string("canvas-override"));
    EXPECT_TRUE(scene.find_object("Extra") != nullptr);
    EXPECT_TRUE(scene.find_object("New") != nullptr);
}

COOPA_TEST(inherit_chain_applies_overrides_nearest_last) {
    write_scratch_file("a.yaml",
        "object:\n"
        "  name: A\n"
        "  components: [ { type: TestTag, label: A-label, extra: A-extra } ]\n");
    write_scratch_file("b.yaml",
        "object:\n"
        "  name: B\n"
        "  inherit_from: a.yaml\n"
        "  components: [ { type: TestTag, label: B-label } ]\n");
    const std::string scene_path = write_scratch_file("scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Final\n"
        "      inherit_from: b.yaml\n"
        "      components: [ { type: TestTag, label: C-label } ]\n");

    coopa::scene::Scene scene = load(scene_path);
    auto tags = find_tags(*scene.find_object("Final"));
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0]->label, std::string("C-label"));
    EXPECT_EQ(tags[0]->extra, std::string("A-extra"));
}

COOPA_TEST(multiple_bases_merge_left_to_right) {
    write_scratch_file("base1.yaml",
        "object:\n"
        "  name: Base1\n"
        "  components: [ { type: TestTag, label: from-base1, extra: e1 } ]\n");
    write_scratch_file("base2.yaml",
        "object:\n"
        "  name: Base2\n"
        "  components: [ { type: TestTag, label: from-base2 } ]\n");
    const std::string scene_path = write_scratch_file("scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Derived\n"
        "      inherit_from: [ base1.yaml, base2.yaml ]\n");

    coopa::scene::Scene scene = load(scene_path);
    auto tags = find_tags(*scene.find_object("Derived"));
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0]->label, std::string("from-base2"));
    EXPECT_EQ(tags[0]->extra, std::string("e1"));
}

COOPA_TEST(inherit_cycle_or_missing_base_throws) {
    write_scratch_file("a.yaml", "object:\n  name: A\n  inherit_from: b.yaml\n");
    write_scratch_file("b.yaml", "object:\n  name: B\n  inherit_from: a.yaml\n");
    const std::string cycle = write_scratch_file("cycle.yaml",
        "scene:\n  root_objects:\n    - name: X\n      inherit_from: a.yaml\n");
    const std::string missing = write_scratch_file("missing.yaml",
        "scene:\n  root_objects:\n    - name: X\n      inherit_from: does_not_exist.yaml\n");

    auto throws_runtime_error = [](const std::string& path) {
        try {
            load(path);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    EXPECT_TRUE(throws_runtime_error(cycle));
    EXPECT_TRUE(throws_runtime_error(missing));
}

COOPA_TEST(asset_paths_resolve_against_the_file_that_declared_them) {
    const std::string note_path = write_scratch_file("prefabs/note.txt", "hi");
    write_scratch_file("prefabs/labeled.yaml",
        "object:\n"
        "  name: Base\n"
        "  components: [ { type: TestAsset, file: note.txt } ]\n");
    const std::string scene_path = write_scratch_file("scenes/scene.yaml",
        "scene:\n"
        "  root_objects:\n"
        "    - name: Widget\n"
        "      inherit_from: ../prefabs/labeled.yaml\n");

    coopa::scene::Scene scene = load(scene_path);
    auto* asset = scene.find_object("Widget")->get_component<TestAssetComponent>();
    ASSERT_TRUE(asset != nullptr);
    // Must resolve against prefabs/ (where note.txt actually lives), not
    // scenes/ (the including scene's own directory, which has no note.txt).
    ASSERT_TRUE(std::filesystem::exists(asset->resolved_path));
    EXPECT_EQ(std::filesystem::canonical(asset->resolved_path).string(),
              std::filesystem::canonical(note_path).string());
}

// read_document_async() resolves on a worker; Builder builds root objects in steps, without
// starting them, and take() hands over an unstarted scene when LoadOptions::start is off.
COOPA_TEST(async_read_and_incremental_build_defer_start) {
    register_test_components();
    write_scratch_file("prefab.yaml",
        "object:\n"
        "  name: Base\n"
        "  components:\n"
        "    - type: TestTag\n"
        "      label: from-prefab\n");
    const std::string scene_path = write_scratch_file("scene.yaml",
        "scene:\n"
        "  scene_name: AsyncBuild\n"
        "  root_objects:\n"
        "    - name: A\n"
        "      components:\n"
        "        - type: TestTag\n"
        "          label: a\n"
        "    - name: B\n"
        "      inherit_from: prefab.yaml\n"
        "    - name: C\n"
        "      components:\n"
        "        - type: TestTag\n"
        "          label: c\n");

    coopa::job::JobEngine jobs(2);
    auto read = coopa::scene::SceneLoader::read_document_async(scene_path, &jobs);
    for (int i = 0; i < 20000 && !read->ready(); ++i) std::this_thread::sleep_for(std::chrono::microseconds(100));
    ASSERT_TRUE(read->ready());
    ASSERT_FALSE(read->failed());

    coopa::scene::SceneLoader::Builder builder(read->document(), scene_path, {.start = false});
    EXPECT_EQ(builder.total(), 3u);
    EXPECT_FALSE(builder.step(0.0)); // a zero budget still builds one root object
    EXPECT_EQ(builder.built(), 1u);
    EXPECT_FALSE(builder.step(0.0));
    EXPECT_TRUE(builder.step(0.0));
    ASSERT_TRUE(builder.done());
    std::unique_ptr<coopa::scene::Scene> scene = builder.take();
    EXPECT_EQ(scene->name(), std::string("AsyncBuild"));
    coopa::scene::SceneObject* b = scene->find_object("B");
    ASSERT_TRUE(b != nullptr);
    ASSERT_EQ(find_tags(*b).size(), 1u);
    EXPECT_EQ(find_tags(*b)[0]->label, std::string("from-prefab")); // inheritance resolved off-thread
    EXPECT_FALSE(find_tags(*b)[0]->started);
    scene->start();
    EXPECT_TRUE(find_tags(*b)[0]->started);

    // load() with start on (the default) is unchanged; a missing file fails the async read.
    coopa::scene::Scene started = coopa::scene::SceneLoader::load(scene_path);
    EXPECT_TRUE(find_tags(*started.find_object("A"))[0]->started);
    coopa::scene::Scene unstarted = coopa::scene::SceneLoader::load(scene_path, {.start = false});
    EXPECT_FALSE(find_tags(*unstarted.find_object("A"))[0]->started);
    auto missing = coopa::scene::SceneLoader::read_document_async(scene_path + ".missing.yaml", &jobs);
    for (int i = 0; i < 20000 && !missing->ready(); ++i) std::this_thread::sleep_for(std::chrono::microseconds(100));
    EXPECT_TRUE(missing->failed());
    EXPECT_FALSE(missing->error().empty());
}
