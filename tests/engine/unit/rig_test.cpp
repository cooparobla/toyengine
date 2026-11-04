/**
 * @file rig_test.cpp
 * @brief Blender-style bone rest poses (head/tail/roll), pose-delta -> model matrix
 *        resolution, and the YAML parser that builds a RigAsset.
 */

#include <coopa/testing/test.h>

#include <memory>
#include <string>

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtc/quaternion.hpp>

#include <coopa/asset/asset_manager.h>
#include <coopa/rig/rig_asset.h>
#include <coopa/rig/rig_component.h>
#include <coopa/rig/rig_loader.h>
#include <coopa/rig/rig_system.h>
#include <coopa/scene/scene.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("rig");

using coopa::rig::Bone;
using coopa::rig::Rig;
using coopa::rig::RigAsset;

namespace {

RigAsset make_two_bone_rig() {
    RigAsset rig;
    rig.name = "test_rig";
    Bone upper;
    upper.name = "upper";
    upper.head = glm::vec3(0.0f, 0.0f, 0.0f);
    upper.tail = glm::vec3(0.0f, 0.0f, 1.0f);
    rig.bones.push_back(upper);
    Bone lower;
    lower.name = "lower";
    lower.parent = 0;
    lower.connected = true;             // head forced onto upper's tail
    lower.head = glm::vec3(0.0f, 0.0f, 99.0f); // authored value is overwritten by compute_rest()
    lower.tail = glm::vec3(0.0f, 0.0f, 2.0f);
    rig.bones.push_back(lower);
    rig.compute_rest();
    return rig;
}

} // namespace

/** @brief A straight-up bone's rest matrix: +Y along head->tail, translation at head, orthonormal. */
COOPA_TEST(bone_rest_matrix_orients_along_head_to_tail) {
    Bone b;
    b.head = glm::vec3(1.0f, 2.0f, 3.0f);
    b.tail = glm::vec3(1.0f, 2.0f, 5.0f); // +Z, length 2
    const glm::mat4 m = coopa::rig::bone_rest_matrix(b);
    const glm::vec3 y(m[1]);
    expect(glm::all(glm::epsilonEqual(y, glm::vec3(0.0f, 0.0f, 1.0f), 1e-5f)), "rest matrix: Y axis is head->tail, normalized");
    expect(glm::all(glm::epsilonEqual(glm::vec3(m[3]), b.head, 1e-5f)), "rest matrix: translation is the head");
    const glm::vec3 x(m[0]), z(m[2]);
    expect_near(glm::length(x), 1.0f, 1e-5f, "rest matrix: X is unit length");
    expect_near(glm::length(z), 1.0f, 1e-5f, "rest matrix: Z is unit length");
    expect_near(glm::dot(x, y), 0.0f, 1e-5f, "rest matrix: X is perpendicular to Y");
    expect(glm::all(glm::epsilonEqual(glm::cross(x, y), z, 1e-5f)), "rest matrix: right-handed (Z = X cross Y)");
}

/** @brief Rolling a bone about its own axis turns X/Z but leaves Y (and the head) untouched. */
COOPA_TEST(roll_turns_the_cross_axes_about_the_bone_axis) {
    Bone b;
    b.head = glm::vec3(0.0f);
    b.tail = glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::mat4 unrolled = coopa::rig::bone_rest_matrix(b);
    b.roll = 90.0f;
    const glm::mat4 rolled = coopa::rig::bone_rest_matrix(b);
    expect(glm::all(glm::epsilonEqual(glm::vec3(unrolled[1]), glm::vec3(rolled[1]), 1e-5f)), "roll: the bone axis itself does not move");
    expect(glm::dot(glm::vec3(unrolled[0]), glm::vec3(rolled[0])) < 0.1f, "roll: 90 degrees swings X most of the way around");
}

/** @brief A `connected` bone's head is forced onto its parent's tail; rest_local decodes back to
 *         the parent-relative matrix compute_rest() derived it from. */
COOPA_TEST(compute_rest_bakes_connected_heads_and_builds_local_matrices) {
    const RigAsset rig = make_two_bone_rig();
    expect(glm::all(glm::epsilonEqual(rig.bones[1].head, glm::vec3(0.0f, 0.0f, 1.0f), 1e-5f)),
           "connected: the child's authored head is overwritten with the parent's tail");
    expect_near(rig.bone_length(0), 1.0f, 1e-5f, "bone_length: upper bone is 1 m");
    expect_near(rig.bone_length(1), 1.0f, 1e-5f, "bone_length: lower bone is 1 m");
    const glm::mat4 reconstructed = rig.rest_model()[0] * rig.rest_local()[1];
    for (int c = 0; c < 4; ++c) {
        expect(glm::all(glm::epsilonEqual(reconstructed[c], rig.rest_model()[1][c], 1e-5f)),
               "rest_local: parent_rest_model * rest_local reconstructs the child's rest_model");
    }
}

/** @brief A pose rotation on the upper bone, with the lower bone left at identity, carries the
 *         lower bone's head with it -- exactly like a rotated parent carrying a child object. */
COOPA_TEST(pose_delta_composes_in_rest_local_frame_and_propagates_to_children) {
    using coopa::scene::Scene;
    using coopa::scene::SceneObject;
    using coopa::scene::TransformComponent;

    coopa::asset::AssetManager assets;
    auto asset = std::make_shared<RigAsset>(make_two_bone_rig());
    auto handle = assets.create<RigAsset>("test/two_bone", asset);

    Scene scene("rig_pose");
    auto obj = std::make_unique<SceneObject>("Rig");
    obj->add_component<TransformComponent>();
    auto* rig = obj->add_component<Rig>();
    rig->asset = handle;
    scene.add_root_object(std::move(obj));
    coopa::rig::install_rig_pose_system(scene);
    scene.start();
    scene.update(0.0f);
    scene.late_update(0.0f);

    // Rest pose first: the lower bone's head sits one meter above the upper bone's.
    expect(glm::all(glm::epsilonEqual(glm::vec3(rig->model_matrix(1)[3]), glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f)),
           "rest pose: the lower bone's resolved head matches the rig's rest_model");

    // Pose the upper bone with an arbitrary rotation, in ITS OWN rest-local frame. The lower
    // bone's resolved head must still be exactly one bone length from the upper bone's resolved
    // head (a rigid rotation preserves distance) and must land where independently recomposing
    // rest_local's translation with that same rotation predicts -- not some other point.
    // Perpendicular to the bone's own axis (local Y) so the lower bone's head, which sits ON
    // that axis in rest_local space, actually swings away from rest rather than barely moving.
    const glm::quat local_rot = glm::angleAxis(glm::radians(37.0f), glm::normalize(glm::vec3(1.0f, 0.0f, 0.3f)));
    rig->pose(0).rotation = local_rot;
    scene.update(0.0f);
    scene.late_update(0.0f);
    const glm::vec3 upper_head(rig->model_matrix(0)[3]);
    const glm::vec3 lower_head(rig->model_matrix(1)[3]);
    expect_near(glm::length(lower_head - upper_head), asset->bone_length(0), 1e-4f,
                "pose: a rigid rotation of the parent preserves its distance to the child's head");
    expect(glm::length(lower_head - glm::vec3(0.0f, 0.0f, 1.0f)) > 0.3f,
           "pose: the lower bone's head actually moved off the rest position");
    const glm::mat3 rest0_rotation(asset->rest_model()[0]);
    const glm::vec3 local_offset(0.0f, asset->bone_length(0), 0.0f); // rest_local's translation is always (0, length, 0)
    const glm::vec3 expected_lower_head = glm::vec3(asset->rest_model()[0][3]) + rest0_rotation * (local_rot * local_offset);
    expect(glm::all(glm::epsilonEqual(lower_head, expected_lower_head, 1e-3f)),
           "pose: the lower head matches rest_local recomposed with the pose rotation, derived independently of Rig::resolve_()");

    // clear_pose() returns both bones to rest.
    rig->clear_pose();
    scene.update(0.0f);
    scene.late_update(0.0f);
    expect(glm::all(glm::epsilonEqual(glm::vec3(rig->model_matrix(1)[3]), glm::vec3(0.0f, 0.0f, 1.0f), 1e-4f)),
           "clear_pose: back to rest");
}

/** @brief A rig's world_matrix() composes the owner object's own Transform, like any other
 *         object in the hierarchy -- so a rig placed and rotated in a scene carries its bones. */
COOPA_TEST(rig_component_resolves_world_matrices_through_the_owner_transform) {
    using coopa::scene::Scene;
    using coopa::scene::SceneObject;
    using coopa::scene::TransformComponent;

    coopa::asset::AssetManager assets;
    auto asset = std::make_shared<RigAsset>(make_two_bone_rig());
    auto handle = assets.create<RigAsset>("test/two_bone_world", asset);

    Scene scene("rig_world");
    auto obj = std::make_unique<SceneObject>("Rig");
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(5.0f, 0.0f, 0.0f));
    auto* rig = obj->add_component<Rig>();
    rig->asset = handle;
    scene.add_root_object(std::move(obj));
    coopa::rig::install_rig_pose_system(scene);
    scene.start();
    scene.update(0.0f);
    scene.late_update(0.0f);

    const glm::vec3 world_head(rig->world_matrix(1)[3]);
    expect(glm::all(glm::epsilonEqual(world_head, glm::vec3(5.0f, 0.0f, 1.0f), 1e-4f)),
           "world_matrix: the owner's offset carries every bone with it");
    const glm::mat4 inv_bind = rig->inverse_bind(1);
    const glm::mat4 identity_check = inv_bind * rig->rest_model_matrix(1);
    for (int c = 0; c < 4; ++c) {
        expect(glm::all(glm::epsilonEqual(identity_check[c], glm::mat4(1.0f)[c], 1e-4f)),
               "inverse_bind: inverse(rest_model) composed with rest_model is identity");
    }
}

/** @brief The rig YAML parser: a parent-first bones list, `parent:` resolved by name, and the
 *         same `connected` head-baking RigAsset::compute_rest() performs. */
COOPA_TEST(parse_rig_reads_head_tail_roll_and_parent_by_name) {
    const fkyaml::node doc = fkyaml::node::deserialize(std::string(
        "rig:\n"
        "  name: arm\n"
        "  bones:\n"
        "    - name: upper\n"
        "      head: {x: 0, y: 0, z: 0}\n"
        "      tail: {x: 0, y: 0, z: 1}\n"
        "    - name: lower\n"
        "      parent: upper\n"
        "      connected: true\n"
        "      tail: {x: 0, y: 0, z: 2}\n"
        "      roll: 45\n"));
    const RigAsset rig = coopa::rig::parse_rig(doc);
    expect(rig.name == "arm", "parse_rig: reads the rig's name");
    require(rig.bones.size() == 2, "parse_rig: reads both bones");
    expect(rig.bones[0].name == "upper" && rig.bones[1].name == "lower", "parse_rig: bone order is preserved");
    expect(rig.bones[1].parent == 0, "parse_rig: 'parent: upper' resolves to upper's index");
    expect_near(rig.bones[1].roll, 45.0f, 1e-5f, "parse_rig: reads roll");
    expect(glm::all(glm::epsilonEqual(rig.bones[1].head, glm::vec3(0.0f, 0.0f, 1.0f), 1e-5f)),
           "parse_rig: connected bakes the child's head onto the parent's tail");
}
