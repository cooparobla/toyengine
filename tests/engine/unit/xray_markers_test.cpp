/**
 * @file xray_markers_test.cpp
 * @brief The x-ray markers: a flag covers its whole subtree, the NEAREST flag wins so a child
 *        can opt back out, subject beats occluder when an object carries both, and the scene
 *        parser reads both keys.
 *
 * These are plain data, but the subtree rule is the part worth pinning: it is what lets a
 * prefab opt its whole tree in at the root, and it is resolved by a hand-written ancestor walk
 * rather than by anything that would notice if it drifted.
 */

#include <coopa/testing/test.h>

#include <memory>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <toyengine/scene/register.h>
#include <toyengine/scene/xray_markers.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("xray_markers");

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using toy::scene::allows_xray;
using toy::scene::is_xray_subject;
using toy::scene::XrayOccluder;
using toy::scene::XrayRole;
using toy::scene::XraySubject;

namespace {

/** @brief A parent with one child and one grandchild, returned deepest-first. */
std::unique_ptr<Scene> make_chain(SceneObject** out_root, SceneObject** out_child,
                                  SceneObject** out_grandchild) {
    auto scene = std::make_unique<Scene>("xray_markers_test");
    auto root = std::make_unique<SceneObject>("root");
    auto child = std::make_unique<SceneObject>("child");
    auto grandchild = std::make_unique<SceneObject>("grandchild");
    *out_grandchild = grandchild.get();
    child->add_child(std::move(grandchild));
    *out_child = child.get();
    root->add_child(std::move(child));
    *out_root = root.get();
    scene->add_root_object(std::move(root));
    return scene;
}

} // namespace

/** @brief An unmarked object plays no part at all -- the feature is opt-in. */
COOPA_TEST(an_unmarked_object_has_no_role) {
    SceneObject* root = nullptr;
    SceneObject* child = nullptr;
    SceneObject* grandchild = nullptr;
    auto scene = make_chain(&root, &child, &grandchild);

    expect(!is_xray_subject(root), "xray: an unmarked object is not a subject");
    expect(!allows_xray(root), "xray: an unmarked object does not allow see-through");
    expect(toy::scene::xray_role(grandchild) == XrayRole::None, "xray: no marker anywhere, no role");
    expect(toy::scene::xray_role(nullptr) == XrayRole::None, "xray: a null object has no role");
}

/** @brief A marker on a parent covers everything under it: one tag opts a whole prefab in. */
COOPA_TEST(a_marker_covers_its_whole_subtree) {
    SceneObject* root = nullptr;
    SceneObject* child = nullptr;
    SceneObject* grandchild = nullptr;
    auto scene = make_chain(&root, &child, &grandchild);
    root->add_component<XraySubject>();

    expect(is_xray_subject(root), "xray: the marked object itself is a subject");
    expect(is_xray_subject(child), "xray: a child of a marked object is a subject");
    expect(is_xray_subject(grandchild), "xray: a grandchild of a marked object is a subject");
    expect(toy::scene::xray_role(grandchild) == XrayRole::Subject, "xray: the role follows the subtree");
}

/**
 * @brief The nearest marker wins, so `enabled: false` on a child is a real opt-out.
 *
 * Without this a prefab is all-or-nothing, and the only way to exclude one part of a rig
 * would be to tag every other part individually.
 */
COOPA_TEST(the_nearest_marker_wins_so_a_child_can_opt_out) {
    SceneObject* root = nullptr;
    SceneObject* child = nullptr;
    SceneObject* grandchild = nullptr;
    auto scene = make_chain(&root, &child, &grandchild);
    root->add_component<XraySubject>();
    child->add_component<XraySubject>()->enabled = false;

    expect(is_xray_subject(root), "xray: the root is still a subject");
    expect(!is_xray_subject(child), "xray: the child opted back out");
    expect(!is_xray_subject(grandchild), "xray: the opt-out covers the child's own subtree");
}

/** @brief The two markers are independent: an object may allow see-through without being a subject. */
COOPA_TEST(the_occluder_marker_is_independent_of_the_subject_marker) {
    SceneObject* root = nullptr;
    SceneObject* child = nullptr;
    SceneObject* grandchild = nullptr;
    auto scene = make_chain(&root, &child, &grandchild);
    root->add_component<XrayOccluder>();

    expect(allows_xray(grandchild), "xray: the occluder marker covers the subtree too");
    expect(!is_xray_subject(grandchild), "xray: an occluder is not thereby a subject");
    expect(toy::scene::xray_role(grandchild) == XrayRole::Occluder, "xray: the role is Occluder");
}

/**
 * @brief Subject beats occluder on an object carrying both.
 *
 * Being seen through yourself is meaningless, and dropping the subject instead would be the
 * harder of the two failures to diagnose -- a player that silently stops showing.
 */
COOPA_TEST(subject_wins_when_an_object_is_both) {
    SceneObject* root = nullptr;
    SceneObject* child = nullptr;
    SceneObject* grandchild = nullptr;
    auto scene = make_chain(&root, &child, &grandchild);
    root->add_component<XraySubject>();
    root->add_component<XrayOccluder>();

    expect(toy::scene::xray_role(root) == XrayRole::Subject, "xray: subject wins over occluder");
}

/** @brief A disabled marker is not a marker, and does not shadow an enabled ancestor's. */
COOPA_TEST(a_disabled_marker_on_its_own_grants_nothing) {
    SceneObject* root = nullptr;
    SceneObject* child = nullptr;
    SceneObject* grandchild = nullptr;
    auto scene = make_chain(&root, &child, &grandchild);
    root->add_component<XrayOccluder>()->enabled = false;

    expect(!allows_xray(root), "xray: a disabled occluder allows nothing");
    expect(!allows_xray(grandchild), "xray: and nothing under it either");
}

/** @brief Both markers round-trip through the scene parser, including the `enabled` key. */
COOPA_TEST(the_markers_round_trip_through_scene_yaml) {
    toy::scene::register_scene_components();
    Scene scene = coopa::scene::SceneLoader::load_from_node(fkyaml::node::deserialize(std::string(
        "scene:\n"
        "  scene_name: xray_markers_yaml\n"
        "  root_objects:\n"
        "    - name: player\n"
        "      components:\n"
        "        - { type: Transform }\n"
        "        - type: XraySubject\n"
        "    - name: ridge\n"
        "      components:\n"
        "        - { type: Transform }\n"
        "        - type: XrayOccluder\n"
        "    - name: vault_door\n"
        "      components:\n"
        "        - { type: Transform }\n"
        "        - type: XrayOccluder\n"
        "          enabled: false\n")), "xray_markers_test.yaml");

    auto* subject = scene.find_first_component<XraySubject>();
    require(subject != nullptr, "xray: the scene parser built an XraySubject");
    expect(subject->enabled, "xray: XraySubject defaults to enabled");

    SceneObject* ridge = scene.find_object("ridge");
    SceneObject* door  = scene.find_object("vault_door");
    require(ridge != nullptr && door != nullptr, "xray: both occluder objects loaded");
    expect(allows_xray(ridge), "xray: the ridge's XrayOccluder parsed as enabled");
    expect(!allows_xray(door), "xray: `enabled: false` parsed");
}
