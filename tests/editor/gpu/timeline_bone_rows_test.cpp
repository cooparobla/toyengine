/**
 * @file timeline_bone_rows_test.cpp
 * @brief The Timeline's bone dope sheet (Pose Mode on a coopa::rig::Rig, over rig_pose_clip_):
 *        entering Pose Mode with the Timeline open draws the bone-clip header instead of the
 *        property one, and select-all / delete / interpolation act on the real, saved clip file
 *        exactly as the property dope sheet's siblings do on a ClipModel.
 */

#include <coopa/testing/test.h>

#include <coopa/rig/rig_component.h>
#include <coopa/rig/skeletal_clip_loader.h>
#include <coopa/yaml/document.h>

#include "editor/anim/bone_clip_model.h"
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("timeline_bone_rows");

namespace toy::editor::testing {

using namespace coopa::test;

namespace {

coopa::rig::Rig* mannequin_rig(EditorApp& app, ObjectId id) {
    auto* live = app.sync().live(id);
    return live ? live->get_component<coopa::rig::Rig>() : nullptr;
}

} // namespace

/** @brief Opening the Timeline while Pose Mode is active on a coopa::rig::Rig draws the bone
 *         dope sheet (no crash, no assertion) and a key inserted via "I" shows up in it. */
COOPA_TEST(timeline_shows_the_bone_clip_after_insert_key) {
    using coopa::input::Key;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    auto& in = session.in;

    Node mannequin = doc.make_object("mannequin");
    mannequin["prefab"] = Node(std::string("objects/characters/mannequin"));
    const ObjectId id = doc.add_object(mannequin);
    app.sync().rebuild(engine, doc);
    app.camera().focus = {0.0f, 0.0f, 1.1f};
    app.camera().distance = 5.0f;
    app.camera().apply();
    tick(engine, 4);

    coopa::rig::Rig* rig = mannequin_rig(app, id);
    coopa::test::require(rig && rig->is_loaded(), "the mannequin's Rig loaded");
    const int chest = rig->find_bone("chest");
    coopa::test::require(chest >= 0, "the rig has a 'chest' bone");

    doc.select(id);
    coopa::test::require(app.set_interaction_mode(InteractionMode::Pose), "Pose Mode opens");
    app.show_timeline();

    glm::vec2 px;
    coopa::test::require(engine.world_to_window(glm::mix(glm::vec3(rig->world_matrix(static_cast<std::size_t>(chest))[3]),
                                                           glm::vec3(rig->world_matrix(static_cast<std::size_t>(chest)) *
                                                                     glm::vec4(0.0f, rig->bone_length(static_cast<std::size_t>(chest)), 0.0f, 1.0f)),
                                                           0.5f),
                                                px),
                        "chest's body projects into the viewport");
    in.click(px / in.scale);
    in.key(Key::G);
    in.key(Key::X);
    in.key(Key::Num1);
    in.key(Key::Enter);
    tick(engine, 2);
    in.key(Key::I);
    tick(engine, 6);   // the Timeline is open: let draw_bone_timeline_/draw_bone_dope_sheet_ run for real

    const auto times = app.bone_clip_key_times();
    expect(!times.empty(), "the Timeline's bone clip model has a key after I (no crash drawing the bone dope sheet either)");

    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief select_all_bone_keys() + delete_selected_bone_keys() removes a keyed bone's keys from
 *         rig_pose_clip_ AND from the file on disk once the per-frame dirty flush runs -- the
 *         bone-clip sibling of the property dope sheet's Delete Keyframes, exercising
 *         rig_pose_clip_edit_()/rig_pose_save_clip_()/timeline_frame_()'s new bookkeeping. */
COOPA_TEST(select_all_and_delete_removes_keys_from_the_saved_clip) {
    using coopa::input::Key;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    auto& in = session.in;

    Node mannequin = doc.make_object("mannequin");
    mannequin["prefab"] = Node(std::string("objects/characters/mannequin"));
    const ObjectId id = doc.add_object(mannequin);
    app.sync().rebuild(engine, doc);
    app.camera().focus = {0.0f, 0.0f, 1.1f};
    app.camera().distance = 5.0f;
    app.camera().apply();
    tick(engine, 4);

    coopa::rig::Rig* rig = mannequin_rig(app, id);
    coopa::test::require(rig && rig->is_loaded(), "the mannequin's Rig loaded");
    const int chest = rig->find_bone("chest");
    coopa::test::require(chest >= 0, "the rig has a 'chest' bone");

    doc.select(id);
    coopa::test::require(app.set_interaction_mode(InteractionMode::Pose), "Pose Mode opens");
    app.show_timeline();

    glm::vec2 px;
    coopa::test::require(engine.world_to_window(glm::mix(glm::vec3(rig->world_matrix(static_cast<std::size_t>(chest))[3]),
                                                           glm::vec3(rig->world_matrix(static_cast<std::size_t>(chest)) *
                                                                     glm::vec4(0.0f, rig->bone_length(static_cast<std::size_t>(chest)), 0.0f, 1.0f)),
                                                           0.5f),
                                                px),
                        "chest's body projects into the viewport");
    in.click(px / in.scale);
    in.key(Key::I);
    tick(engine, 2);
    coopa::test::require(!app.bone_clip_key_times().empty(), "I keyed the chest bone");

    const fs::path saved = doc.path().parent_path() / "animations" / "mannequin" / "idle.yaml";
    coopa::test::require(coopa::yaml::document_exists(saved), "I saved the scratch idle.yaml: " + saved.string());
    {
        const BoneClipModel before_delete = BoneClipModel::from_node(coopa::yaml::load_document(saved));
        const BoneClipTrack* chest_track = before_delete.find_bone_track("chest");
        coopa::test::require(chest_track && !chest_track->location.keys.empty(), "...and the saved file has the chest key before deleting");
    }

    app.select_all_bone_keys();
    app.delete_selected_bone_keys();
    tick(engine, 4);   // timeline_frame_() flushes rig_pose_clip_dirty_ on the next frame, not inline

    expect(app.bone_clip_key_times().empty(), "delete_selected_bone_keys() cleared the in-memory bone clip model");
    const BoneClipModel after_delete = BoneClipModel::from_node(coopa::yaml::load_document(saved));
    const BoneClipTrack* chest_track_after = after_delete.find_bone_track("chest");
    expect(!chest_track_after || (chest_track_after->location.keys.empty() && chest_track_after->rotation.keys.empty() &&
                                   chest_track_after->scale.keys.empty()),
           "...and the SAVED file no longer has the chest key either (the per-frame dirty flush actually ran)");

    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief set_selected_bone_key_interpolation() writes the easing onto a saved key, read back
 *         through the exact runtime parser -- the bone-clip sibling of the property dope
 *         sheet's key-menu Interpolation entries. */
COOPA_TEST(interpolation_change_round_trips_through_the_saved_clip) {
    using coopa::input::Key;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    auto& in = session.in;

    Node mannequin = doc.make_object("mannequin");
    mannequin["prefab"] = Node(std::string("objects/characters/mannequin"));
    const ObjectId id = doc.add_object(mannequin);
    app.sync().rebuild(engine, doc);
    app.camera().focus = {0.0f, 0.0f, 1.1f};
    app.camera().distance = 5.0f;
    app.camera().apply();
    tick(engine, 4);

    coopa::rig::Rig* rig = mannequin_rig(app, id);
    coopa::test::require(rig && rig->is_loaded(), "the mannequin's Rig loaded");
    const int chest = rig->find_bone("chest");
    coopa::test::require(chest >= 0, "the rig has a 'chest' bone");

    doc.select(id);
    coopa::test::require(app.set_interaction_mode(InteractionMode::Pose), "Pose Mode opens");
    app.show_timeline();

    glm::vec2 px;
    coopa::test::require(engine.world_to_window(glm::mix(glm::vec3(rig->world_matrix(static_cast<std::size_t>(chest))[3]),
                                                           glm::vec3(rig->world_matrix(static_cast<std::size_t>(chest)) *
                                                                     glm::vec4(0.0f, rig->bone_length(static_cast<std::size_t>(chest)), 0.0f, 1.0f)),
                                                           0.5f),
                                                px),
                        "chest's body projects into the viewport");
    in.click(px / in.scale);
    in.key(Key::I);
    tick(engine, 2);
    coopa::test::require(!app.bone_clip_key_times().empty(), "I keyed the chest bone");

    app.select_all_bone_keys();
    app.set_selected_bone_key_interpolation("ease_in_out");
    tick(engine, 4);

    const fs::path saved = doc.path().parent_path() / "animations" / "mannequin" / "idle.yaml";
    const BoneClipModel reloaded = BoneClipModel::from_node(coopa::yaml::load_document(saved));
    const BoneClipTrack* chest_track = reloaded.find_bone_track("chest");
    coopa::test::require(chest_track && !chest_track->location.keys.empty(), "the saved clip still has the chest location key");
    expect(chest_track->location.keys[0].easing == "ease_in_out",
           "...now written as ease_in_out (got '" + chest_track->location.keys[0].easing + "')");

    const coopa::rig::SkeletalClip rt = coopa::rig::parse_skeletal_clip(coopa::yaml::load_document(saved));
    bool rt_found = false;
    for (const auto& t : rt.bones) if (t.bone == "chest") rt_found = !t.location.keys().empty();
    expect(rt_found, "...and the runtime's own skeletal clip parser still reads it");

    app.set_interaction_mode(InteractionMode::Object);
}

} // namespace toy::editor::testing
