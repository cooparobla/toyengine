/**
 * @file bone_clip_model_test.cpp
 * @brief BoneClipModel against the runtime's skeletal clip parser: keying per bone/channel, a
 *        lossless YAML round trip, and events.
 */

#include <coopa/testing/test.h>

#include <toyengine/rig/skeletal_clip_loader.h>

#include "editor/anim/bone_clip_model.h"

COOPA_TEST_SUITE("bone_clip_model");

namespace toy::editor::testing {

using namespace coopa::test;

/** @brief set_key adds/replaces per-bone/channel keys, and key_times reads them back sorted. */
COOPA_TEST(set_key_adds_and_replaces_keys_per_bone_and_channel) {
    BoneClipModel m;
    m.name = "walk";
    m.set_key("pelvis", "location", 0.0f, glm::vec4(0, 0, 0.9f, 0));
    m.set_key("pelvis", "location", 0.5f, glm::vec4(0, 0, 1.0f, 0));
    m.set_key("chest", "rotation", 0.0f, glm::vec4(0, 0, 0, 1));
    expect(m.find_bone_track("pelvis") && m.find_bone_track("pelvis")->location.keys.size() == 2,
           "two distinct times keep two keys on pelvis.location");
    m.set_key("pelvis", "location", 0.5f, glm::vec4(0, 0, 1.2f, 0));
    expect(m.find_bone_track("pelvis")->location.keys.size() == 2, "re-keying an existing time replaces it, not adds");
    expect_near(m.find_bone_track("pelvis")->location.keys[1].value.z, 1.2f, 1e-6f, "...with the new value");
    const auto times = m.key_times("pelvis", "location");
    expect(times.size() == 2 && times[0] < times[1], "key_times reads them back sorted");
    expect(m.find_bone_track("chest")->rotation.keys.size() == 1, "a different bone/channel gets its own track");
}

/** @brief to_node()/from_node() round-trips name/wrap/length, bone channels, root_motion and events
 *         through the exact text the runtime's parse_skeletal_clip() reads. */
COOPA_TEST(lossless_round_trip_through_the_runtime_parser) {
    BoneClipModel m;
    m.name = "walk";
    m.wrap = "loop";
    m.length = 0.9f;
    m.root_motion.bone = "pelvis";
    m.root_motion.translation = "xy";
    m.root_motion.yaw = true;
    m.set_key("pelvis", "location", 0.0f, glm::vec4(0, 0, 0.9f, 0));
    m.set_key("pelvis", "location", 0.45f, glm::vec4(0, 1.2f, 0.95f, 0));
    m.set_key("pelvis", "rotation", 0.0f, glm::vec4(0, 0, 0, 1));
    m.set_key("chest", "rotation", 0.0f, glm::vec4(0, 0, 0.1f, 0.995f));
    m.add_event(0.2333f, "footstep");

    const Node node = m.to_node();
    const BoneClipModel back = BoneClipModel::from_node(node);
    expect(back == m, "from_node(to_node()) reproduces the model exactly");

    // The runtime's own parser must read the same written text the same way.
    const toy::rig::SkeletalClip rt = toy::rig::parse_skeletal_clip(node);
    expect(rt.name == "walk" && rt.wrap == toy::anim::WrapMode::Loop, "the runtime parser reads name/wrap");
    expect(rt.root_motion.bone == "pelvis" && rt.root_motion.yaw, "...and root_motion (bone + yaw)");
    expect(rt.events.size() == 1 && rt.events[0].name == "footstep", "...and the event");
    bool found_pelvis = false;
    for (const auto& t : rt.bones) if (t.bone == "pelvis") found_pelvis = !t.location.keys().empty();
    expect(found_pelvis, "...and the pelvis bone's location keys");
}

/** @brief The dope-sheet cell ops (move/delete/easing), addressed by BoneKeyRef(bone, channel,
 *         time) exactly like ClipModel's KeyRef(object, property, time). */
COOPA_TEST(dope_sheet_cell_ops_move_delete_and_easing) {
    using Ref = BoneClipModel::BoneKeyRef;
    BoneClipModel m;
    m.set_key("pelvis", "location", 0.0f, glm::vec4(0, 0, 0.9f, 0));
    m.set_key("pelvis", "location", 0.5f, glm::vec4(0, 0, 1.0f, 0));
    m.set_key("pelvis", "rotation", 0.5f, glm::vec4(0, 0, 0, 1));
    m.set_key("chest", "rotation", 0.5f, glm::vec4(0, 0, 0.1f, 0.995f));

    expect(m.key_times("pelvis").size() == 2, "key_times(bone) aggregates across its channels (0.0 and 0.5)");
    expect(m.key_times().size() == 2, "key_times() aggregates across every bone too");

    const size_t n = m.set_easing({Ref{"pelvis", 0.5f, "location"}}, "ease_in");
    expect(n == 1, "set_easing with a channel ref touches only that channel's key");
    expect(m.find_bone_track("pelvis")->location.keys[1].easing == "ease_in", "...and sets it");
    expect(m.find_bone_track("pelvis")->rotation.keys[0].easing.empty(), "...leaving the sibling channel's key at this same time alone");
    expect(m.easing_of({Ref{"pelvis", 0.5f, "location"}}) == "ease_in", "easing_of reads it back");

    const size_t whole_bone = m.set_easing({Ref{"pelvis", 0.5f, ""}}, "step");
    expect(whole_bone == 2, "a whole-bone ref (empty channel) touches every channel's key at that time");

    expect_near(m.neighbour_key(0.0f, +1), 0.5f, 1e-6f, "neighbour_key finds the next key across bones");
    expect_near(m.neighbour_key(0.5f, -1), 0.0f, 1e-6f, "...and the previous one");

    const auto moved = m.move_keys({Ref{"pelvis", 0.5f, ""}}, 0.2f);
    expect(moved.size() == 1, "a whole-bone ref collapses to one moved cell (channel \"\"), same as ClipModel's whole-object move");
    expect(m.key_times("pelvis", "location")[1] > 0.69f, "the moved location key lands at the new time");
    expect(m.key_times("pelvis", "rotation")[0] > 0.69f, "...and the moved rotation key too");

    const size_t deleted = m.delete_keys({Ref{"chest", 0.5f, "rotation"}});
    expect(deleted == 1 && m.find_bone_track("chest") == nullptr, "deleting a bone's only key drops the now-empty track");
}

/** @brief Events stay sorted by time; add/move/delete work the same as the property side's. */
COOPA_TEST(events_stay_sorted_and_edit) {
    BoneClipModel m;
    const size_t i0 = m.add_event(0.5f, "a");
    expect(i0 == 0, "the first event ever added lands at index 0");
    const size_t i1 = m.add_event(0.1f, "b");
    expect(i1 == 0, "a later-added, earlier-time event is inserted before the first");
    expect(m.events[0].name == "b" && m.events[1].name == "a", "...so the stored order is by time");
    const int moved = m.move_event(0, 0.9f);
    expect(moved == 1 && m.events[1].name == "b", "moving the earlier event past the later one re-sorts it");
    expect(m.delete_event(0) && m.events.size() == 1, "delete_event removes by index");
}

} // namespace toy::editor::testing
