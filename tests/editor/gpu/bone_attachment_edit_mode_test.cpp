/**
 * @file bone_attachment_edit_mode_test.cpp
 * @brief A rigged character's bone-parented mesh parts must be SPREAD ALONG THE RIG in the
 *        editor, not piled at its origin.
 *
 * A bone is not a SceneObject, so the scene graph alone cannot place a rig's parts -- only
 * BoneAttachment (order 345, after RigPoseSystem at 340) can. Both of those systems therefore
 * have to run while the scene is NOT simulating, which is exactly how the editor holds it
 * (Scene::set_simulating(false)). When they did not, every part of the mannequin collapsed onto
 * its authored local offset at the rig's own origin and the editor drew a blob of capsules
 * instead of a humanoid -- with no error, no failing test, and a correct-looking rig asset.
 */

#include <coopa/testing/test.h>

#include <coopa/rig/bone_attachment.h>
#include <coopa/rig/rig_component.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("bone_attachment_edit_mode");

namespace toy::editor::testing {

using namespace coopa::test;

/** @brief The mannequin prefab, placed in the editor's (non-simulating) scene: its parts ride
 *         their bones, so the set of part positions spans the rig's full height. */
COOPA_TEST(bone_parented_parts_ride_their_bones_in_edit_mode) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    Node mannequin = doc.make_object("mannequin");
    mannequin["prefab"] = Node(std::string("objects/characters/mannequin"));
    const ObjectId id = doc.add_object(mannequin);
    app.sync().rebuild(engine, doc);
    tick(engine, 8);

    coopa::test::require(!engine.scene().is_simulating(), "the editor holds the scene NOT simulating (the whole point here)");

    auto* live = app.sync().live(id);
    coopa::test::require(live != nullptr, "the mannequin instance is live");
    auto* rig = live->get_component<coopa::rig::Rig>();
    coopa::test::require(rig && rig->is_loaded(), "its Rig loaded");

    // Every BoneAttachment under it must have resolved to its bone...
    std::vector<coopa::rig::BoneAttachment*> attachments;
    for (coopa::rig::BoneAttachment* a : engine.scene().get_components<coopa::rig::BoneAttachment>()) attachments.push_back(a);
    coopa::test::require(attachments.size() >= 4, "the mannequin has several bone-parented parts (got " +
                                                      std::to_string(attachments.size()) + ")");
    for (coopa::rig::BoneAttachment* a : attachments) {
        expect(a->rig() != nullptr && a->bone_index() >= 0,
               "BoneAttachment on bone '" + a->bone + "' resolved its rig and bone index in edit mode");
    }

    // ...and the parts' WORLD positions must span the rig, not sit in a pile at its origin.
    float lo = 1e9f, hi = -1e9f;
    for (coopa::rig::BoneAttachment* a : attachments) {
        auto* tc = a->owner ? a->owner->get_transform() : nullptr;
        if (!tc) continue;
        const float z = glm::vec3(tc->get_world_matrix()[3]).z;
        lo = std::min(lo, z);
        hi = std::max(hi, z);
    }
    // The mannequin is ~1.8 m tall; pelvis sits near 0.95 m and the head near 1.6 m, so a
    // correctly-placed set spans well over half a metre. A collapsed pile spans ~0.
    expect(hi - lo > 0.5f, "the parts span the rig's height rather than piling at its origin (span " +
                               std::to_string(hi - lo) + " m)");
    expect(hi > 1.0f, "...reaching up the rig (highest part at z " + std::to_string(hi) + " m)");
}

} // namespace toy::editor::testing
