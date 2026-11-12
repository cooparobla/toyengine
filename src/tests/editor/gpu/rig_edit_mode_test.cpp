/**
 * @file rig_edit_mode_test.cpp
 * @brief Rig Edit mode authoring a BRAND NEW rig from scratch through real input: add a root
 *        bone, extrude a child, drag its tail endpoint, set its roll, rename it (checking a
 *        referring clip gets rewritten), and delete a bone with a child (checking the child
 *        reparents) -- all verified against the ACTUAL SAVED FILE through the real
 *        toy::rig::parse_rig() loader, not just in-memory state.
 */

#include <coopa/testing/test.h>

#include <toyengine/rig/rig_loader.h>
#include <toyengine/rig/skeletal_clip_loader.h>
#include <coopa/yaml/document.h>

#include "editor/anim/bone_clip_model.h"
#include "editor/rig/rig_model.h"
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("rig_edit_mode");

namespace toy::editor::testing {

using namespace coopa::test;

/** @brief Rig Edit is the Edit mode of a RIGGED object and nothing else: a bare object is
 *         refused (there is no skeleton to edit), and Add > Armature is what makes one. */
COOPA_TEST(rig_edit_opens_only_on_a_rigged_object) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    Node obj = doc.make_object("new_character");
    const ObjectId bare = doc.add_object(obj);
    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    doc.select(bare);
    expect(!app.set_interaction_mode(InteractionMode::RigEdit), "Rig Edit is REFUSED on an object with no Rig");
    expect(app.interaction_mode() != InteractionMode::RigEdit, "...and the mode did not change");
}

/** @brief Add > Armature creates the object, its Rig component and its own rigs/<name>.yaml with
 *         one root bone, and leaves you in Rig Edit on it -- Blender's "add an armature, then
 *         Tab into it", and the only route by which a rig comes into being. */
COOPA_TEST(add_armature_creates_a_rig_and_enters_edit) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    tick(engine, 4);

    const ObjectId id = app.create_armature();
    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    coopa::test::require(id != 0, "Add > Armature created an object");
    expect(app.interaction_mode() == InteractionMode::RigEdit, "...and dropped straight into Rig Edit on it");
    expect(app.rig_edit_model().bones.size() == 1, "...with exactly one root bone to start from");
    coopa::test::require(doc.find_component(id, "Rig") >= 0, "...a Rig component was added");

    const fs::path saved = app.rig_edit_path();
    coopa::test::require(!saved.empty() && coopa::yaml::document_exists(saved),
                        "...and its rig file exists on disk: " + saved.string());
    const toy::rig::RigAsset rt = toy::rig::parse_rig(coopa::yaml::load_document(saved));
    expect(rt.bones.size() == 1, "the runtime's own rig parser reads the one bone back");

    // The gate now passes for this object, which is the point of the whole flow.
    app.set_interaction_mode(InteractionMode::Object);
    doc.select(id);
    expect(app.set_interaction_mode(InteractionMode::Edit),
           "Edit mode on the armature re-enters Rig Edit (no separate mode to pick)");
    expect(app.interaction_mode() == InteractionMode::RigEdit, "...reporting Rig Edit");
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

    const ObjectId id = app.create_armature();   // seeds one root bone and enters Rig Edit
    app.sync().rebuild(engine, doc);
    app.camera().focus = {0.0f, 0.0f, 0.5f};
    app.camera().distance = 4.0f;
    app.camera().apply();
    tick(engine, 4);

    doc.select(id);
    coopa::test::require(app.interaction_mode() == InteractionMode::RigEdit, "Rig Edit mode is open");
    coopa::test::require(app.rig_edit_model().bones.size() == 1, "the armature starts with one root bone");

    // The keyboard-only shortcuts below are gated on the viewport being hovered (see
    // viewport_keymap_'s own call site) -- unlike every other interaction in this file, this
    // one has no preceding click to put the cursor there incidentally, so it must move there
    // explicitly first (the same pattern animation_test.cpp/viewport_test.cpp use).
    in.move(glm::vec2(app.viewport_box().center()));
    app.select_rig_bone(0);
    coopa::test::require(app.rig_edit_selected_bone() == 0, "the root bone is selected");

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

    const fs::path saved = session.project.assets() / "rigs" / "armature.yaml";
    coopa::test::require(coopa::yaml::document_exists(saved), "Rig Edit saved rigs/armature.yaml: " + saved.string());
    const toy::rig::RigAsset asset = toy::rig::parse_rig(coopa::yaml::load_document(saved));
    expect(asset.bones.size() == 2, "the saved file has both bones");
    expect_near(asset.bones[1].tail.z, tail_after.z, 1e-3f, "...with the dragged tail position");

    auto* live = app.sync().live(id);
    coopa::test::require(live != nullptr, "the object is still live");
    auto* rig = live->get_component<toy::rig::Rig>();
    coopa::test::require(rig != nullptr, "...and has its Rig component");
    expect(rig->rig_path == "rigs/armature.yaml", "...pointing at the file Rig Edit saved");
}

/** @brief set_selected_rig_bone_roll() writes a numeric roll that round-trips through the
 *         saved file and the runtime's own rest-matrix math. */
COOPA_TEST(numeric_roll_round_trips_through_the_saved_file) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    const ObjectId id = app.create_armature();
    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    doc.select(id);
    coopa::test::require(app.interaction_mode() == InteractionMode::RigEdit, "Rig Edit mode is open");
    coopa::test::require(app.rig_edit_model().bones.size() == 1, "the armature's root bone is there");
    app.select_rig_bone(0);
    app.set_selected_rig_bone_roll(37.0f);
    expect_near(app.rig_edit_model().bones[0].roll, 37.0f, 1e-4f, "the roll took effect in memory");

    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 2);
    const fs::path saved = session.project.assets() / "rigs" / "armature.yaml";
    coopa::test::require(coopa::yaml::document_exists(saved), "the rig saved: " + saved.string());
    const toy::rig::RigAsset asset = toy::rig::parse_rig(coopa::yaml::load_document(saved));
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

    const toy::rig::RigAsset reloaded = toy::rig::parse_rig(coopa::yaml::load_document(rig_path));
    expect(reloaded.find_bone("forearm") >= 0 && reloaded.find_bone("upper_arm") < 0,
           "the saved rig file itself has the new name, not the old one");

    coopa::test::require(coopa::yaml::document_exists(clip_path), "the clip file is still there");
    const BoneClipModel reloaded_clip = BoneClipModel::from_node(coopa::yaml::load_document(clip_path));
    expect(reloaded_clip.find_bone_track("forearm") != nullptr, "the clip's track was rewritten to the new bone name");
    expect(reloaded_clip.find_bone_track("upper_arm") == nullptr, "...and the old name is gone from it");
    const toy::rig::SkeletalClip rt = toy::rig::parse_skeletal_clip(coopa::yaml::load_document(clip_path));
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

    const ObjectId id = app.create_armature();
    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    doc.select(id);
    coopa::test::require(app.interaction_mode() == InteractionMode::RigEdit, "Rig Edit mode is open");
    app.select_rig_bone(0);                 // bone 0: the armature's root
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

    const fs::path saved = session.project.assets() / "rigs" / "armature.yaml";
    coopa::test::require(coopa::yaml::document_exists(saved), "the rig saved: " + saved.string());
    const toy::rig::RigAsset asset = toy::rig::parse_rig(coopa::yaml::load_document(saved));
    coopa::test::require(asset.bones.size() == 2, "the saved file also has two bones");
    const int saved_gc = asset.find_bone(grandchild_name);
    coopa::test::require(saved_gc >= 0, "...including the reparented one");
    expect(asset.bones[static_cast<size_t>(saved_gc)].parent == 0, "...with parent index 0 in the saved file too");
}

/** @brief Ctrl+Z in Rig Edit undoes the BONE edit and leaves the scene document alone -- it used
 *         to fall through to the scene's own undo stack, which could delete the object being
 *         edited out from under the mode. Ctrl+Shift+Z puts the bone edit back. */
COOPA_TEST(ctrl_z_undoes_the_bone_edit_not_the_scene) {
    using coopa::input::Key;
    using coopa::input::Mods;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    auto& in = session.in;

    const ObjectId id = app.create_armature();
    app.sync().rebuild(engine, doc);
    tick(engine, 4);
    doc.select(id);
    coopa::test::require(app.interaction_mode() == InteractionMode::RigEdit, "Rig Edit mode is open");
    const std::size_t objects_before = doc.all_ids().size();

    app.select_rig_bone(0);
    const glm::vec3 tail_before = app.rig_edit_model().bones[0].tail;
    app.set_selected_rig_bone_tail(tail_before + glm::vec3(0.0f, 0.0f, 0.5f));
    app.end_rig_edit_merge();
    tick(engine, 2);
    coopa::test::require(app.rig_edit_model().bones[0].tail.z > tail_before.z + 0.4f, "the bone edit landed");

    in.move(glm::vec2(app.viewport_box().center()));
    in.key(Key::Z, Mods::Control);
    tick(engine, 2);
    expect_near(app.rig_edit_model().bones[0].tail.z, tail_before.z, 1e-4f, "Ctrl+Z restored the bone's tail");
    expect(doc.all_ids().size() == objects_before,
           "...and the SCENE document is untouched (the armature was not undone away)");
    expect(doc.find(id) != nullptr, "...the object being edited still exists");

    in.key(Key::Z, Mods::Control | Mods::Shift);
    tick(engine, 2);
    expect(app.rig_edit_model().bones[0].tail.z > tail_before.z + 0.4f, "Ctrl+Shift+Z redoes the bone edit");
    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief Blender's selection model end to end: shift-click extends, a body click takes both
 *         endpoints while an endpoint click takes one, and G moves exactly what is selected. */
COOPA_TEST(multi_select_and_endpoint_grab) {
    using coopa::input::Key;
    using coopa::input::Mods;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    auto& in = session.in;

    const ObjectId id = app.create_armature();
    app.select_rig_bone(0);
    app.extrude_selected_rig_bone();                // a second bone, connected
    app.sync().rebuild(engine, doc);
    app.camera().focus = {0.0f, 0.0f, 0.4f};
    app.camera().distance = 3.0f;
    app.camera().apply();
    tick(engine, 4);
    doc.select(id);
    coopa::test::require(app.interaction_mode() == InteractionMode::RigEdit, "Rig Edit mode is open");
    coopa::test::require(app.rig_edit_model().bones.size() == 2, "two bones to select between");

    // A body click selects both of that bone's endpoints and makes it active.
    const RigModel& m = app.rig_edit_model();
    const glm::vec3 mid0 = glm::mix(m.bones[0].head, m.bones[0].tail, 0.5f);
    glm::vec2 px;
    coopa::test::require(engine.world_to_window(mid0, px), "bone 0's body projects into the viewport");
    in.click(px / in.scale);
    tick(engine, 2);
    const auto& sel = app.rig_edit_selection();
    expect(sel.bones[0].head && sel.bones[0].tail, "a body click selects BOTH endpoints");
    expect(sel.active == 0, "...and makes that bone active");
    expect(!sel.bones[1].head && !sel.bones[1].tail, "...leaving the other bone alone");

    // Shift-clicking the second bone's body extends the selection to it.
    const glm::vec3 mid1 = glm::mix(m.bones[1].head, m.bones[1].tail, 0.5f);
    coopa::test::require(engine.world_to_window(mid1, px), "bone 1's body projects into the viewport");
    in.click(px / in.scale, coopa::input::MouseButton::Left, Mods::Shift);
    tick(engine, 2);
    expect(sel.bones[0].both() && sel.bones[1].both(), "shift-click EXTENDS to a second bone");

    // G Z 1 Enter now moves every selected endpoint by the typed delta.
    const glm::vec3 b0_head = m.bones[0].head, b1_tail = m.bones[1].tail;
    in.key(Key::G);
    in.key(Key::Z);
    in.key(Key::Num1);
    in.key(Key::Enter);
    tick(engine, 2);
    expect_near(m.bones[0].head.z - b0_head.z, 1.0f, 0.02f, "G Z 1 moved the first bone's head");
    expect_near(m.bones[1].tail.z - b1_tail.z, 1.0f, 0.02f, "...and the second bone's tail: the whole selection");

    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief Symmetrize through the real operator, verified in the SAVED file: an `_l` bone's `_r`
 *         partner is created mirrored across X. */
COOPA_TEST(symmetrize_writes_the_mirrored_side_to_the_file) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    const ObjectId id = app.create_armature();
    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    doc.select(id);
    coopa::test::require(app.interaction_mode() == InteractionMode::RigEdit, "Rig Edit mode is open");

    app.select_rig_bone(0);
    app.rename_selected_rig_bone("arm_l");
    app.set_selected_rig_bone_head({0.2f, 0.0f, 0.0f});
    app.set_selected_rig_bone_tail({0.7f, 0.0f, 0.0f});
    app.end_rig_edit_merge();
    app.select_rig_bone(0);
    app.symmetrize_rig();
    tick(engine, 2);

    const int r = app.rig_edit_model().find_bone("arm_r");
    coopa::test::require(r >= 0, "Symmetrize created the _r partner");
    expect_near(app.rig_edit_model().bones[static_cast<size_t>(r)].head.x, -0.2f, 1e-4f, "...mirrored across X");

    app.set_interaction_mode(InteractionMode::Object);   // leaving saves
    tick(engine, 2);
    const fs::path saved = session.project.assets() / "rigs" / "armature.yaml";
    coopa::test::require(coopa::yaml::document_exists(saved), "the rig saved: " + saved.string());
    const toy::rig::RigAsset asset = toy::rig::parse_rig(coopa::yaml::load_document(saved));
    const int saved_r = asset.find_bone("arm_r");
    coopa::test::require(saved_r >= 0, "...and the runtime's own parser reads the mirrored bone");
    expect_near(asset.bones[static_cast<size_t>(saved_r)].head.x, -0.2f, 1e-3f, "...at the mirrored position");
}

/** @brief On an object carrying BOTH a rig and a mesh of its own, Tab cycles
 *         Object -> Rig Edit -> Mesh Edit -> Object: there is no separate Rig Edit mode row to
 *         pick, because editing a rig IS what Edit means for one. */
COOPA_TEST(tab_cycles_object_rig_edit_mesh_edit) {
    using coopa::input::Key;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    auto& in = session.in;

    // create_armature() makes the Rig; give that same object a mesh of its own so it has both.
    const ObjectId id = app.create_armature();
    app.set_interaction_mode(InteractionMode::Object);
    Node renderer = default_component("MeshRenderer");
    renderer["mesh_path"] = Node(std::string("cube"));
    doc.add_component(id, renderer);
    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    doc.select(id);
    coopa::test::require(app.interaction_mode() == InteractionMode::Object, "starting in Object Mode");
    in.move(glm::vec2(app.viewport_box().center()));   // Tab is keyboard-only: the viewport must be hovered

    in.key(Key::Tab);
    tick(engine, 2);
    expect(app.interaction_mode() == InteractionMode::RigEdit,
           "Tab from Object goes to RIG Edit first -- the rig is what Edit means here");

    in.key(Key::Tab);
    tick(engine, 2);
    expect(app.interaction_mode() == InteractionMode::Edit, "...a second Tab moves on to the mesh's Edit Mode");

    in.key(Key::Tab);
    tick(engine, 2);
    expect(app.interaction_mode() == InteractionMode::Object, "...and a third closes the cycle back to Object");

    // On an object with a rig and NO mesh, the same key is a two-way toggle.
    const ObjectId bare = app.create_armature();
    app.set_interaction_mode(InteractionMode::Object);
    app.sync().rebuild(engine, doc);
    tick(engine, 4);
    doc.select(bare);
    in.move(glm::vec2(app.viewport_box().center()));
    in.key(Key::Tab);
    tick(engine, 2);
    expect(app.interaction_mode() == InteractionMode::RigEdit, "a rig-only object Tabs into Rig Edit");
    in.key(Key::Tab);
    tick(engine, 2);
    expect(app.interaction_mode() == InteractionMode::Object, "...and straight back out, with no mesh step");
}

} // namespace toy::editor::testing
