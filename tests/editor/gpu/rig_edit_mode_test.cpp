/**
 * @file rig_edit_mode_test.cpp
 * @brief Rig Edit mode authoring a BRAND NEW rig from scratch through real input: add a root
 *        bone, extrude a child, drag its tail endpoint, set its roll, rename it (checking a
 *        referring clip gets rewritten), and delete a bone with a child (checking the child
 *        reparents) -- all verified against the ACTUAL SAVED FILE through the real
 *        coopa::rig::parse_rig() loader, not just in-memory state.
 */

#include <coopa/testing/test.h>

#include <coopa/rig/rig_loader.h>
#include <coopa/rig/skeletal_clip_loader.h>
#include <coopa/yaml/document.h>

#include "editor/anim/bone_clip_model.h"
#include "editor/rig/rig_model.h"
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("rig_edit_mode");

namespace toy::editor::testing {

using namespace coopa::test;

/** @brief Entering Rig Edit mode on a bare, rig-less object starts an EMPTY, unsaved
 *         RigDocument (authoring from scratch is the whole point of this mode). */
COOPA_TEST(rig_edit_mode_opens_on_a_bare_object_with_no_rig_yet) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    Node obj = doc.make_object("new_character");
    const ObjectId id = doc.add_object(obj);
    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    doc.select(id);
    expect(app.set_interaction_mode(InteractionMode::RigEdit), "Rig Edit mode opens on a plain object");
    expect(app.interaction_mode() == InteractionMode::RigEdit, "...and reports Rig Edit mode");
    expect(app.rig_edit_model().bones.empty(), "...with an empty rig to author from scratch");
    expect(app.rig_edit_path().empty(), "...and no file yet (nothing has been saved)");
    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief Shift+A adds a root bone; E extrudes a connected child from its tail; G on the
 *         child's picked tail endpoint moves it by exactly the typed world-space delta. The
 *         first edit saves a rigs/<object-name>.yaml AND adds a Rig component pointing at it,
 *         both verified by reloading the file through the real runtime parser. */
COOPA_TEST(add_extrude_and_drag_an_endpoint_builds_and_saves_a_real_rig) {
    using coopa::input::Key;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    auto& in = session.in;

    Node obj = doc.make_object("new_character");
    const ObjectId id = doc.add_object(obj);
    app.sync().rebuild(engine, doc);
    app.camera().focus = {0.0f, 0.0f, 0.5f};
    app.camera().distance = 4.0f;
    app.camera().apply();
    tick(engine, 4);

    doc.select(id);
    coopa::test::require(app.set_interaction_mode(InteractionMode::RigEdit), "Rig Edit mode opens");

    // The keyboard-only shortcuts below are gated on the viewport being hovered (see
    // viewport_keymap_'s own call site) -- unlike every other interaction in this file, this
    // one has no preceding click to put the cursor there incidentally, so it must move there
    // explicitly first (the same pattern animation_test.cpp/viewport_test.cpp use).
    in.move(glm::vec2(app.viewport_box().center()));
    in.key(Key::A, coopa::input::Mods::Shift);
    tick(engine, 2);
    coopa::test::require(app.rig_edit_model().bones.size() == 1, "Shift+A added one root bone");
    coopa::test::require(app.rig_edit_selected_bone() == 0, "...and selected it");

    in.key(Key::E);
    tick(engine, 2);
    coopa::test::require(app.rig_edit_model().bones.size() == 2, "E extruded a second bone");
    const int child = app.rig_edit_selected_bone();
    coopa::test::require(child == 1, "...selecting the new child");
    expect(app.rig_edit_model().bones[1].connected, "...which starts connected to the root's tail");

    // Click the child's tail endpoint (its own projected position) to select it specifically,
    // then G Z 1 Enter moves it by exactly 1m along world Z (no animation here to drift the
    // click target, unlike Pose mode's idle-driven rigs -- this is pure rest data).
    const glm::vec3 tail_before = app.rig_edit_model().bones[static_cast<size_t>(child)].tail;
    glm::vec2 px;
    coopa::test::require(engine.world_to_window(tail_before, px), "the child's tail projects into the viewport");
    in.click(px / in.scale);
    in.key(Key::G);
    in.key(Key::Z);
    in.key(Key::Num1);
    in.key(Key::Enter);
    tick(engine, 2);

    const glm::vec3 tail_after = app.rig_edit_model().bones[static_cast<size_t>(child)].tail;
    expect_near(tail_after.z - tail_before.z, 1.0f, 0.02f,
               "G Z 1 Enter on the picked tail moves it exactly 1m along world Z (got " +
                   std::to_string(tail_after.z - tail_before.z) + ")");
    expect_near(tail_after.x - tail_before.x, 0.0f, 0.02f, "...and nothing on X");
    expect_near(tail_after.y - tail_before.y, 0.0f, 0.02f, "...and nothing on Y");

    app.set_interaction_mode(InteractionMode::Object);   // leaving the mode saves
    tick(engine, 2);

    const fs::path saved = session.project.assets() / "rigs" / "new_character.yaml";
    coopa::test::require(coopa::yaml::document_exists(saved), "Rig Edit saved rigs/new_character.yaml: " + saved.string());
    const coopa::rig::RigAsset asset = coopa::rig::parse_rig(coopa::yaml::load_document(saved));
    expect(asset.bones.size() == 2, "the saved file has both bones");
    expect_near(asset.bones[1].tail.z, tail_after.z, 1e-3f, "...with the dragged tail position");

    auto* live = app.sync().live(id);
    coopa::test::require(live != nullptr, "the object is still live");
    auto* rig = live->get_component<coopa::rig::Rig>();
    coopa::test::require(rig != nullptr, "...and now has a Rig component (added on first save)");
    expect(rig->rig_path == "rigs/new_character.yaml", "...pointing at the file Rig Edit just saved");
}

/** @brief set_selected_rig_bone_roll() writes a numeric roll that round-trips through the
 *         saved file and the runtime's own rest-matrix math. */
COOPA_TEST(numeric_roll_round_trips_through_the_saved_file) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    Node obj = doc.make_object("roll_rig");
    const ObjectId id = doc.add_object(obj);
    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    doc.select(id);
    coopa::test::require(app.set_interaction_mode(InteractionMode::RigEdit), "Rig Edit mode opens");
    app.add_rig_bone();
    coopa::test::require(app.rig_edit_model().bones.size() == 1, "a root bone was added");
    app.set_selected_rig_bone_roll(37.0f);
    expect_near(app.rig_edit_model().bones[0].roll, 37.0f, 1e-4f, "the roll took effect in memory");

    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 2);
    const fs::path saved = session.project.assets() / "rigs" / "roll_rig.yaml";
    coopa::test::require(coopa::yaml::document_exists(saved), "the rig saved: " + saved.string());
    const coopa::rig::RigAsset asset = coopa::rig::parse_rig(coopa::yaml::load_document(saved));
    coopa::test::require(!asset.bones.empty(), "the saved asset has the bone");
    expect_near(asset.bones[0].roll, 37.0f, 1e-3f, "...with the same roll the runtime's own parser reads");
}

/** @brief Renaming a bone rewrites a referring clip's bone track -- the name changes in the
 *         CLIP file on disk, read back through the runtime's own SkeletalClip parser. */
COOPA_TEST(rename_rewrites_a_referring_clip) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    Node obj = doc.make_object("renamer");
    Node rig_comp = Node::mapping();
    rig_comp["type"] = Node(std::string("Rig"));
    rig_comp["rig"] = Node(std::string("rigs/renamer_rig.yaml"));
    Node anim_comp = Node::mapping();
    anim_comp["type"] = Node(std::string("SkeletalAnimator"));
    Node state = Node::mapping();
    state["name"] = Node(std::string("idle"));
    state["clip"] = Node(std::string("animations/renamer/idle.yaml"));
    anim_comp["states"] = Node::sequence();
    anim_comp["states"].as_seq().push_back(state);
    obj["components"].as_seq().push_back(rig_comp);     // appended after make_object()'s own Transform
    obj["components"].as_seq().push_back(anim_comp);
    const ObjectId id = doc.add_object(obj);
    coopa::test::require(app.save_scene(), "the scene saves to disk");   // the referrer sweep below walks FILES, not the in-memory document

    // Write the rig asset and the referring clip by hand first, so Rig Edit opens an EXISTING
    // rig (pre-populated from disk) rather than authoring a new one -- this is the "rename an
    // already-saved rig" path, the one the referrer rewrite actually matters for.
    RigModel seed;
    seed.name = "renamer_rig";
    seed.add_bone("upper_arm", -1, {0, 0, 1}, {0, 0, 1.3f});
    const fs::path project_assets = session.project.assets();
    const fs::path rig_path = project_assets / "rigs" / "renamer_rig.yaml";
    fs::create_directories(rig_path.parent_path());
    coopa::yaml::save_document(rig_path, seed.to_node());

    BoneClipModel clip;
    clip.name = "idle";
    clip.set_key("upper_arm", "rotation", 0.0f, glm::vec4(0, 0, 0, 1));
    const fs::path clip_path = project_assets / "animations" / "renamer" / "idle.yaml";
    fs::create_directories(clip_path.parent_path());
    coopa::yaml::save_document(clip_path, clip.to_node());

    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    doc.select(id);
    coopa::test::require(app.set_interaction_mode(InteractionMode::RigEdit), "Rig Edit mode opens");
    coopa::test::require(app.rig_edit_model().bones.size() == 1, "the pre-existing rig loaded with its one bone");
    coopa::test::require(app.rig_edit_path() == rig_path, "...from the file on disk, not a fresh one");

    app.add_rig_bone();   // bone 1: a second bone, to prove a rename can collide with it
    coopa::test::require(app.rig_edit_model().bones.size() == 2, "a second bone got added");
    app.select_rig_bone(1);
    app.rename_selected_rig_bone("upper_arm");   // should fail: name taken by bone 0
    expect(app.rig_edit_model().bones[1].name != "upper_arm", "renaming to an already-taken name was rejected");

    app.select_rig_bone(0);
    app.reparent_selected_rig_bone(-1);   // no-op content-wise (already root); just exercises the call
    app.rename_selected_rig_bone("forearm");
    const int renamed = app.rig_edit_model().find_bone("forearm");
    coopa::test::require(renamed >= 0, "the rename took effect in memory");

    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 2);

    const coopa::rig::RigAsset reloaded = coopa::rig::parse_rig(coopa::yaml::load_document(rig_path));
    expect(reloaded.find_bone("forearm") >= 0 && reloaded.find_bone("upper_arm") < 0,
           "the saved rig file itself has the new name, not the old one");

    coopa::test::require(coopa::yaml::document_exists(clip_path), "the clip file is still there");
    const BoneClipModel reloaded_clip = BoneClipModel::from_node(coopa::yaml::load_document(clip_path));
    expect(reloaded_clip.find_bone_track("forearm") != nullptr, "the clip's track was rewritten to the new bone name");
    expect(reloaded_clip.find_bone_track("upper_arm") == nullptr, "...and the old name is gone from it");
    const coopa::rig::SkeletalClip rt = coopa::rig::parse_skeletal_clip(coopa::yaml::load_document(clip_path));
    bool rt_found = false;
    for (const auto& t : rt.bones) if (t.bone == "forearm") rt_found = !t.rotation.keys().empty();
    expect(rt_found, "...and the runtime's own clip parser reads the renamed track");
}

/** @brief Deleting the MIDDLE bone of a three-bone chain reparents its child onto the deleted
 *         bone's own parent -- Blender's rule -- verified in the saved file. */
COOPA_TEST(delete_bone_reparents_its_child_in_the_saved_file) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    Node obj = doc.make_object("delete_rig");
    const ObjectId id = doc.add_object(obj);
    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    doc.select(id);
    coopa::test::require(app.set_interaction_mode(InteractionMode::RigEdit), "Rig Edit mode opens");
    app.add_rig_bone();                     // bone 0: root
    app.extrude_selected_rig_bone();        // bone 1: child of 0
    app.extrude_selected_rig_bone();        // bone 2: child of 1
    coopa::test::require(app.rig_edit_model().bones.size() == 3, "three bones exist");
    coopa::test::require(app.rig_edit_model().bones[1].parent == 0 && app.rig_edit_model().bones[2].parent == 1,
                        "a straight chain: 0 <- 1 <- 2");
    const std::string grandchild_name = app.rig_edit_model().bones[2].name;

    app.select_rig_bone(1);   // the middle bone
    app.delete_selected_rig_bone();
    coopa::test::require(app.rig_edit_model().bones.size() == 2, "deleting the middle bone leaves two");
    const int gc = app.rig_edit_model().find_bone(grandchild_name);
    coopa::test::require(gc >= 0, "the former grandchild is still there");
    expect(app.rig_edit_model().bones[static_cast<size_t>(gc)].parent == 0,
           "...now reparented directly onto the root (the deleted bone's own parent)");

    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 2);

    const fs::path saved = session.project.assets() / "rigs" / "delete_rig.yaml";
    coopa::test::require(coopa::yaml::document_exists(saved), "the rig saved: " + saved.string());
    const coopa::rig::RigAsset asset = coopa::rig::parse_rig(coopa::yaml::load_document(saved));
    coopa::test::require(asset.bones.size() == 2, "the saved file also has two bones");
    const int saved_gc = asset.find_bone(grandchild_name);
    coopa::test::require(saved_gc >= 0, "...including the reparented one");
    expect(asset.bones[static_cast<size_t>(saved_gc)].parent == 0, "...with parent index 0 in the saved file too");
}

} // namespace toy::editor::testing
