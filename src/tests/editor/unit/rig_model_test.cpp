/**
 * @file rig_model_test.cpp
 * @brief RigModel against the runtime's rig asset parser: add/extrude/move/roll/rename/
 *        reparent/connect/delete, and a lossless YAML round trip.
 */

#include <coopa/testing/test.h>

#include <toyengine/rig/rig_loader.h>

#include "editor/rig/rig_model.h"

COOPA_TEST_SUITE("rig_model");

namespace toy::editor::testing {

using namespace coopa::test;

/** @brief add_bone/extrude_bone build a parent-first chain; set_tail cascades a connected
 *         child's head along with it, and set_head on a connected bone moves its parent's tail. */
COOPA_TEST(extrude_and_move_cascade_through_connected_bones) {
    RigModel m;
    const int root = m.add_bone("pelvis", -1, {0, 0, 0}, {0, 0, 0.2f});
    const int spine = m.extrude_bone(root, "spine", {0, 0, 0.5f});
    expect(spine >= 0, "extrude_bone succeeds from a valid parent");
    expect(m.bones[static_cast<size_t>(spine)].connected, "an extruded bone starts connected");
    expect(glm::all(glm::equal(m.bones[static_cast<size_t>(spine)].head, glm::vec3(0, 0, 0.2f))),
           "...with its head at the extrude source's tail");

    m.set_tail(root, {0, 0, 0.3f});
    expect(glm::all(glm::equal(m.bones[static_cast<size_t>(spine)].head, glm::vec3(0, 0, 0.3f))),
           "moving pelvis's tail carries spine's (connected) head along with it");

    m.set_head(spine, {0, 0, 0.4f});
    expect(glm::all(glm::equal(m.bones[static_cast<size_t>(root)].tail, glm::vec3(0, 0, 0.4f))),
           "moving a connected bone's head actually moves its PARENT's tail (the same point)");
    expect(glm::all(glm::equal(m.bones[static_cast<size_t>(spine)].head, glm::vec3(0, 0, 0.4f))),
           "...which cascades back onto spine's own head");
}

/** @brief set_connected(true) pins a head to the parent's tail once; false just clears the flag. */
COOPA_TEST(set_connected_pins_or_releases_the_head) {
    RigModel m;
    const int root = m.add_bone("pelvis", -1, {0, 0, 0}, {0, 0, 0.2f});
    const int child = m.add_bone("spine", root, {1, 1, 1}, {1, 1, 1.5f});   // not touching the parent's tail
    m.set_connected(child, true);
    expect(glm::all(glm::equal(m.bones[static_cast<size_t>(child)].head, glm::vec3(0, 0, 0.2f))),
           "turning connected on pins the head to the parent's tail immediately");
    m.set_connected(child, false);
    expect(!m.bones[static_cast<size_t>(child)].connected, "turning it off just clears the flag");
    expect(glm::all(glm::equal(m.bones[static_cast<size_t>(child)].head, glm::vec3(0, 0, 0.2f))),
           "...without moving the head back");
}

/** @brief rename_bone rejects an empty or already-taken name; reparent_bone enforces parent-first
 *         order and rejects a cycle. */
COOPA_TEST(rename_and_reparent_validate) {
    RigModel m;
    const int a = m.add_bone("a", -1, {0, 0, 0}, {0, 0, 1});
    const int b = m.add_bone("b", -1, {1, 0, 0}, {1, 0, 1});
    expect(m.rename_bone(a, "a2"), "a plain rename succeeds");
    expect(m.bones[static_cast<size_t>(a)].name == "a2", "...and takes effect");
    expect(!m.rename_bone(b, "a2"), "renaming to an already-taken name fails");
    expect(!m.rename_bone(a, ""), "renaming to an empty name fails");

    expect(m.reparent_bone(b, a), "reparenting under an EARLIER bone succeeds");
    expect(m.bones[static_cast<size_t>(b)].parent == a, "...and takes effect");
    const int c = m.add_bone("c", b, {1, 0, 1}, {1, 0, 2});
    expect(!m.reparent_bone(a, c), "reparenting under a LATER bone fails (parent-first order)");
    expect(!m.reparent_bone(a, b), "...and so does reparenting a onto its own descendant b (a cycle)");
}

/** @brief delete_bone reparents its children to its own parent, matching Blender; a connected
 *         child's head follows onto the grandparent's tail (or disconnects at the root). */
COOPA_TEST(delete_bone_reparents_children) {
    RigModel m;
    const int root = m.add_bone("pelvis", -1, {0, 0, 0}, {0, 0, 0.2f});
    const int mid = m.extrude_bone(root, "spine", {0, 0, 0.5f});
    const int tip = m.extrude_bone(mid, "chest", {0, 0, 0.8f});
    const auto reparented = m.delete_bone(mid);
    expect(reparented.size() == 1 && reparented[0] == "chest", "deleting spine reports chest as reparented");
    const int chest_after = m.find_bone("chest");
    coopa::test::require(chest_after >= 0, "chest still exists after the delete");
    expect(m.bones[static_cast<size_t>(chest_after)].parent == root, "...now parented directly to pelvis");
    expect(glm::all(glm::equal(m.bones[static_cast<size_t>(chest_after)].head, glm::vec3(0, 0, 0.2f))),
           "...and its (connected) head followed onto pelvis's tail");
    expect(m.find_bone("spine") == -1, "spine itself is gone");
    (void)tip;
}

/** @brief to_node()/from_node() round-trips through the exact text toy::rig::parse_rig() reads,
 *         including the rest-matrix math (bone_rest_matrix) agreeing on both sides. */
COOPA_TEST(lossless_round_trip_through_the_runtime_parser) {
    RigModel m;
    m.name = "test_rig";
    const int root = m.add_bone("pelvis", -1, {0, 0, 0.9f}, {0, 0.1f, 0.9f});
    const int spine = m.extrude_bone(root, "spine", {0, 0, 1.2f});
    m.set_roll(spine, 15.0f);

    const Node node = m.to_node();
    const RigModel back = RigModel::from_node(node);
    expect(back == m, "from_node(to_node()) reproduces the model exactly");

    const toy::rig::RigAsset asset = toy::rig::parse_rig(node);
    expect(asset.name == "test_rig", "the runtime parser reads the name");
    expect(asset.bones.size() == 2, "...and both bones");
    const int rt_spine = asset.find_bone("spine");
    coopa::test::require(rt_spine >= 0, "...finds spine by name");
    expect(asset.bones[static_cast<size_t>(rt_spine)].parent == 0, "...with pelvis (index 0) as its parent");
    expect_near(asset.bones[static_cast<size_t>(rt_spine)].roll, 15.0f, 1e-4f, "...and the same roll");

    const glm::mat4 editor_world = m.world_rest_matrix(spine);
    const glm::mat4 runtime_world = asset.rest_model()[static_cast<size_t>(rt_spine)];
    expect(glm::all(glm::equal(glm::vec3(editor_world[3]), glm::vec3(runtime_world[3]))),
           "the editor's own rest-matrix math agrees with the runtime's on spine's world position");
}

} // namespace toy::editor::testing
