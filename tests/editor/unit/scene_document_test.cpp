/**
 * @file scene_document_test.cpp
 * @brief SceneDocument, the editor's model of a scene: the O(1) id index agrees with the tree after
 * every structural edit, undo/redo of any edit sequence is exact, reparenting refuses cycles,
 * SaveId identities stay unique, and created names are snake_case and unique.
 */

#include <coopa/testing/test.h>

#include <functional>
#include <map>
#include <random>
#include <set>

#include "editor/app/project.h"
#include "editor/core/naming.h"
#include "editor/core/scene_document.h"
#include "editor/schema/component_schema.h"
#include "editor/support/fixtures.h"

COOPA_TEST_SUITE("scene_document");

namespace toy::editor::testing {

/**
 * @brief SceneDocument's id index (find / parent_of / is_ancestor in O(1)) agrees with a plain
 *        walk of the tree after every kind of structural change: add (nested), reparent,
 *        duplicate, delete, undo, redo, a copy of the document, and a save + load.
 */
COOPA_TEST(id_index_matches_the_tree_after_every_structural_edit) {
    SceneDocument doc;
    doc.reset("index");
    // Expected (id -> parent) from a fresh walk of the document.
    auto walk = [](const SceneDocument& d) {
        std::map<ObjectId, ObjectId> parents;
        std::function<void(const Node&, ObjectId)> rec = [&](const Node& list, ObjectId parent) {
            if (!list.is_sequence()) return;
            for (const auto& o : list.as_seq()) {
                const ObjectId id = SceneDocument::id_of(o);
                parents[id] = parent;
                if (o.is_mapping() && o.contains("children")) rec(o.at("children"), id);
            }
        };
        rec(d.root_objects(), 0);
        return parents;
    };
    auto check = [&](const SceneDocument& d, const std::string& when) {
        const auto parents = walk(d);
        bool ok = true;
        for (const auto& [id, parent] : parents) {
            const Node* n = d.find(id);
            ok &= n != nullptr && SceneDocument::id_of(*n) == id;
            ok &= d.parent_of(id).value_or(-1) == parent;
            for (ObjectId a = parent; a != 0; a = parents.at(a)) ok &= d.is_ancestor(a, id);
            ok &= d.is_ancestor(id, id);
        }
        ok &= d.find(987654321) == nullptr && !d.parent_of(987654321).has_value();
        expect(ok, "the id index matches the tree " + when);
    };

    std::vector<ObjectId> roots;
    for (int i = 0; i < 6; ++i) roots.push_back(doc.add_object(doc.make_object("r" + std::to_string(i))));
    const ObjectId kid = doc.add_object(doc.make_object("kid"), roots[1]);
    const ObjectId grandkid = doc.add_object(doc.make_object("grandkid"), kid);
    check(doc, "after nested adds");
    doc.reparent(kid, roots[4]);
    check(doc, "after a reparent");
    expect(doc.is_ancestor(roots[4], grandkid) && !doc.is_ancestor(roots[1], grandkid), "ancestry follows the reparent");
    const auto copies = doc.duplicate_objects({roots[4], roots[0]});
    check(doc, "after a duplicate");
    expect(copies.size() == 2 && doc.find(copies[0]) && doc.find(copies[1]), "duplicates are findable");
    doc.delete_objects({roots[2], kid});
    check(doc, "after a delete");
    expect(!doc.find(kid) && !doc.find(grandkid), "deleted subtrees are gone from the index");
    doc.undo();
    check(doc, "after undo");
    expect(doc.find(grandkid) && doc.parent_of(grandkid).value_or(0) == kid, "undo brings the subtree back");
    doc.redo();
    check(doc, "after redo");
    // A copy carries no index into the original's tree.
    SceneDocument copy = doc;
    copy.delete_objects({roots[0]});
    check(copy, "in a copy after its own edit");
    check(doc, "in the original after the copy's edit");
    expect(doc.find(roots[0]) && !copy.find(roots[0]), "copy and original are independent");
    // Save + load.
    const fs::path file = coopa::test::scratch_dir("id_index") / "scene.yaml";
    doc.save(file);
    SceneDocument loaded;
    loaded.load(file);
    check(loaded, "after a load");
    expect(loaded.all_ids().size() == doc.all_ids().size(), "a reload keeps every object");
}

COOPA_TEST(undoing_and_redoing_random_edits_is_exact) {
    SceneDocument doc;
    doc.load(fs::path(ROOT_DIR) / "assets/scenes/demos/pixel_demo/scene.yaml");
    const Node original = doc.node();
    std::mt19937 rng(1234);
    int edits = 0;
    for (int i = 0; i < 200; ++i) {
        const auto ids = doc.all_ids();
        if (ids.empty()) break;
        const ObjectId a = ids[rng() % ids.size()];
        const ObjectId b = ids[rng() % ids.size()];
        Change c;
        switch (rng() % 6) {
            case 0: c = {ChangeScope::Structure, doc.add_object(doc.make_object("N" + std::to_string(i)), rng() % 2 ? a : 0)}; break;
            case 1: { auto d = doc.duplicate_objects({a}); c = {d.empty() ? ChangeScope::None : ChangeScope::Structure, 0}; break; }
            case 2: c = doc.delete_objects({a}); break;
            case 3: c = doc.reparent(a, rng() % 3 ? b : 0); break;
            case 4: c = doc.set_transform(a, glm::vec3(i, 1, 2), glm::vec3(0, 0, i), glm::vec3(1), "T"); break;
            default: c = doc.set_object_key(a, "name", Node("R" + std::to_string(i)), "Rename"); break;
        }
        if (c.scope != ChangeScope::None) ++edits;
    }
    const Node final_state = doc.node();
    expect(edits > 100, "most random edits applied (" + std::to_string(edits) + ")");
    while (doc.undo_stack().can_undo()) doc.undo();
    expect(doc.node() == original, "undoing everything restores the original document");
    while (doc.undo_stack().can_redo()) doc.redo();
    expect(doc.node() == final_state, "redoing everything restores the final document");
}

COOPA_TEST(reparent_refuses_cycles_and_duplicates_get_fresh_ids) {
    SceneDocument doc;
    doc.reset("T");
    const ObjectId p = doc.add_object(doc.make_object("Parent"));
    const ObjectId c = doc.add_object(doc.make_object("Child"), p);
    const ObjectId g = doc.add_object(doc.make_object("Grandchild"), c);
    expect(doc.parent_of(g) == c && doc.parent_of(c) == p && doc.parent_of(p) == ObjectId(0), "parents are tracked");
    expect(doc.reparent(p, g).scope == ChangeScope::None, "an object cannot move under its own descendant");
    expect(doc.reparent(p, p).scope == ChangeScope::None, "or under itself");
    expect(doc.reparent(g, 0).scope == ChangeScope::Structure && doc.parent_of(g) == ObjectId(0), "moving to the root works");
    expect(doc.unique_name("Parent") == "Parent_001", "unique names number in snake_case style (_001)");
    auto dup = doc.duplicate_objects({c});
    expect(dup.size() == 1 && dup[0] != c && doc.find(dup[0]) != nullptr, "duplicates get fresh ids");
}

/**
 * @brief SaveId identities stay unique in the editor: adding a SaveId fills in a fresh id (and
 *        replaces one already taken), duplicating an object -- or a parent of one -- gives every
 *        copy's SaveId a new id, and the ids survive a save + reload.
 */
COOPA_TEST(save_ids_stay_unique_through_duplicate_and_reload) {
    SceneDocument doc;
    doc.reset("save_ids");
    auto save_id_of = [&](ObjectId id) {
        const int idx = doc.find_component(id, "SaveId");
        return idx < 0 ? std::string() : get_string(doc.find(id)->at("components").as_seq()[static_cast<size_t>(idx)], "id");
    };
    const ObjectId chest = doc.add_object(doc.make_object("Old Chest"));
    doc.add_component(chest, default_component("SaveId"));
    const std::string first = save_id_of(chest);
    expect(first.rfind("old_chest_", 0) == 0 && first.size() == 16, "adding a SaveId fills in a readable unique id (" + first + ")");

    const ObjectId other = doc.add_object(doc.make_object("door"));
    Node taken = default_component("SaveId");
    taken["id"] = Node(first);
    doc.add_component(other, taken);
    expect(!save_id_of(other).empty() && save_id_of(other) != first, "adding a SaveId with a taken id gets a fresh one");
    const ObjectId kept = doc.add_object(doc.make_object("gate"));
    Node mine = default_component("SaveId");
    mine["id"] = Node(std::string("main_gate"));
    doc.add_component(kept, mine);
    expect(save_id_of(kept) == "main_gate", "an unused authored id is kept");

    const ObjectId kid = doc.add_object(doc.make_object("coin"), kept);
    doc.add_component(kid, default_component("SaveId"));
    const auto copies = doc.duplicate_objects({chest, kept});
    expect(copies.size() == 2, "duplicated two objects");
    std::set<std::string> ids;
    int count = 0;
    doc.for_each_object([&](const Node& o, int) {
        if (!o.contains("components")) return;
        for (const auto& c : o.at("components").as_seq()) {
            if (component_type(c) == "SaveId") { ids.insert(get_string(c, "id")); ++count; }
        }
    });
    expect(count == 7 && ids.size() == 7, "every SaveId is unique after duplicating (children too): " + std::to_string(ids.size()) + "/" + std::to_string(count));
    expect(save_id_of(chest) == first && save_id_of(kept) == "main_gate", "the originals keep their ids");

    const fs::path file = coopa::test::scratch_dir("save_ids") / "scene.yaml";
    doc.save(file);
    SceneDocument loaded;
    loaded.load(file);
    std::set<std::string> reloaded;
    loaded.for_each_object([&](const Node& o, int) {
        if (!o.contains("components")) return;
        for (const auto& c : o.at("components").as_seq()) if (component_type(c) == "SaveId") reloaded.insert(get_string(c, "id"));
    });
    expect(reloaded == ids, "SaveIds survive a save + reload");
}

COOPA_TEST(created_names_are_snake_case_and_unique) {
    // What the editor creates is named like assets/: snake_case.
    expect(snake_case("Point Light") == "point_light" && snake_case("ReflectionProbe") == "reflection_probe" &&
           snake_case("my-Asset 2") == "my_asset_2" && snake_case("HTTPServer") == "http_server" &&
           snake_case("already_snake") == "already_snake" && snake_case("  Wood Crate! ") == "wood_crate",
           "snake_case() handles spaces, camelCase, acronyms and punctuation");
    SceneDocument doc;
    doc.reset("t");
    doc.add_object(doc.make_object("cube"));
    expect(doc.unique_name("cube") == "cube_001", "a taken name gets _001");
    doc.add_object(doc.make_object("cube_001"));
    expect(doc.unique_name("cube_001") == "cube_002", "duplicating cube_001 renumbers it (cube_002, not cube_001_001)");
    expect(doc.unique_name("Cube.001") == "Cube.001", "a free name is kept exactly as typed");

    const Node scene = Project::default_scene_node("main");
    std::vector<std::string> names;
    for (const auto& o : scene.at("scene").at("root_objects").as_seq()) names.push_back(get_string(o, "name"));
    expect(get_string(scene.at("scene"), "scene_name") == "main" && names == std::vector<std::string>{"camera", "sun", "ground", "cube"},
           "the default scene and its objects are snake_case");
}

} // namespace toy::editor::testing
