/**
 * @file animation_test.cpp
 * @brief Animation in the editor: an Animator makes a rig, clips are files of that object's, Record
 * poses only the live rig and auto-keys exactly the moved channels, scrubbing interpolates, events
 * and keys undo with the file following, renaming moves the file, rigs preview only their own
 * clips, and object-asset rigs play when placed.
 */

#include <coopa/testing/test.h>

#include <coopa/animation/animation_clip_loader.h>

#include "editor/anim/clip_model.h"
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("animation");

namespace toy::editor::testing {

/** @brief Animation end to end: an Animator makes a rig, a clip is a file of that object's,
 *         Record poses without touching the rest pose, keys interpolate when scrubbed, the rest
 *         pose comes back, Ctrl+Z undoes a key, and Play runs the clip. */
COOPA_TEST(record_keys_scrub_events_and_play) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    const ObjectId cube = object_named(app, "cube");
    const ObjectId bone = doc.add_object(doc.make_object("Bone"), cube);
    app.sync().rebuild(engine, doc);
    tick(engine, 2);

    doc.select(cube);
    app.show_timeline();
    app.add_animator(cube);
    tick(engine, 2);
    expect(app.animation_rig() == cube, "an Animator makes the object a rig");
    app.new_animation_clip("Wave");
    tick(engine, 2);
    const fs::path scene_dir = doc.path().parent_path();
    expect(app.animation_clip() && fs::exists(scene_dir / "animations" / "cube" / "Wave.yaml"),
           "a new clip is a file under animations/<rig>/");
    const Node& anim = doc.find(cube)->at("components").as_seq()[static_cast<size_t>(doc.find_component(cube, "Animator"))];
    expect(get_string(anim, "auto_play") == "Wave" && anim.at("states").as_seq().size() == 1,
           "...listed as the Animator's state, and played on start");

    // Record two keys on the bone.
    doc.select(bone);
    app.set_animation_record(true);
    tick(engine, 2);
    expect(app.animation_rig() == cube, "selecting a bone keeps its rig");
    glm::vec3 rp, rr, rs;
    doc.get_transform(bone, rp, rr, rs);
    app.set_animation_time(0.0f);
    tick(engine, 1);
    app.set_object_transform(bone, glm::vec3(0, 0, 1), glm::vec3(0), glm::vec3(1));
    app.insert_keyframes();
    app.set_animation_time(1.0f);
    tick(engine, 1);
    app.set_object_transform(bone, glm::vec3(2, 0, 1), glm::vec3(0, 0, 90), glm::vec3(1));
    app.insert_keyframes();
    tick(engine, 1);
    glm::vec3 dp, dr, ds;
    doc.get_transform(bone, dp, dr, ds);
    expect(dp == rp && dr == rr, "Record poses the live rig only: the document's rest pose is untouched");
    const ClipModel* clip = app.animation_clip();
    expect(clip && clip->find_track("Bone", "position") && clip->find_track("Bone", "position")->keys.size() == 2 &&
               clip->find_track("Bone", "rotation_quat"),
           "I keys position, rotation (quaternion) and scale on the bone's path");

    auto live_bone = [&]() -> coopa::scene::SceneObject* { return app.sync().live(bone); };
    app.set_animation_time(0.5f);
    tick(engine, 2);
    const auto& lt = live_bone()->get_transform()->transform();
    const glm::quat q = lt.rotation_quat();
    expect(std::abs(lt.position().x - 1.0f) < 1e-3f, "scrubbing to the middle interpolates the position");
    expect(std::abs(glm::degrees(2.0f * std::atan2(std::abs(q.z), q.w)) - 45.0f) < 0.5f, "...and the rotation (45 deg)");
    dump(engine, "17_timeline");

    // Ctrl+Z (recording): the last keys go -- the move's auto-key and then I's -- from the file too.
    app.undo();
    app.undo();
    tick(engine, 1);
    const ClipModel saved = ClipModel::from_node(coopa::yaml::load_document(scene_dir / "animations" / "cube" / "Wave.yaml"));
    expect(app.animation_clip()->find_track("Bone", "position")->keys.size() == 1 &&
               saved.find_track("Bone", "position") && saved.find_track("Bone", "position")->keys.size() == 1,
           "undo removes the last keys, and the file follows");
    app.redo();
    app.redo();
    tick(engine, 1);

    // The Events lane: add, drag (snapped to frames), rename, delete -- each one undo step, and
    // the file (and so the runtime clip) follows.
    app.add_animation_event(0.5f, "footstep");
    app.add_animation_event(0.2f, "whoosh");
    tick(engine, 1);
    expect(app.animation_clip()->events.size() == 2 && app.animation_clip()->events[0].name == "whoosh" &&
               app.selected_animation_event() == 0,
           "events: added in time order, the new one selected");
    dump(engine, "17b_timeline_events");
    app.select_animation_event(1);
    app.move_selected_animation_event(0.71f);   // snaps to frame 21 (0.7 s)
    app.rename_selected_animation_event("step_left");
    tick(engine, 1);
    const ClipModel with_events = ClipModel::from_node(coopa::yaml::load_document(scene_dir / "animations" / "cube" / "Wave.yaml"));
    expect(with_events.events.size() == 2 && with_events.events[1].name == "step_left" &&
               std::abs(with_events.events[1].time - 0.7f) < 1e-4f,
           "events: dragged (snapped) and renamed, saved to the clip file");
    const auto rt = coopa::anim::parse_clip(coopa::yaml::load_document(scene_dir / "animations" / "cube" / "Wave.yaml"));
    expect(rt.events.size() == 2 && rt.events[1].name == "step_left", "events: the runtime parses the editor's events");
    app.delete_selected_animation_event();
    tick(engine, 1);
    expect(app.animation_clip()->events.size() == 1, "events: deleted");
    app.undo();   // the delete
    app.undo();   // the rename
    tick(engine, 1);
    expect(app.animation_clip()->events.size() == 2 && app.animation_clip()->events[1].name == "footstep",
           "events: undo restores the deleted event, then its old name");
    app.undo();   // the move
    app.undo();   // the second add
    tick(engine, 1);
    expect(app.animation_clip()->events.size() == 1 && app.animation_clip()->events[0].name == "footstep" &&
               std::abs(app.animation_clip()->events[0].time - 0.5f) < 1e-4f,
           "events: ...the move and the add undo too");
    app.undo();
    tick(engine, 1);
    expect(app.animation_clip()->events.empty(), "events: back to none");

    // The rest pose returns when the clip is not shown.
    app.set_animation_record(false);
    app.set_timeline_rest_pose(true);
    tick(engine, 2);
    expect(glm::distance(live_bone()->get_transform()->transform().position(), rp) < 1e-5f,
           "Rest Pose puts the authored transform back on the live rig");
    app.set_timeline_rest_pose(false);

    // Play: the Animator plays the clip file.
    app.play();
    tick(engine, 40);
    auto* played = engine.scene().find_object("Bone");
    expect(app.playing() && played && played->get_transform()->transform().position().x > 0.2f,
           "in Play the Animator runs the clip (Bone x = " +
               std::to_string(played ? played->get_transform()->transform().position().x : -1.0f) + ")");
    app.stop();
    tick(engine, 2);
}

/** @brief Animating the friendly way: Record auto-keys exactly the channels a move changes (a
 *         drag of many moves is one undo step), the inspector's diamonds key one channel, the key
 *         menu's interpolation reaches the file and the sampling, and a clip renames cleanly. */
COOPA_TEST(autokey_channels_interpolation_and_rename) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    const ObjectId cube = object_named(app, "cube");
    const ObjectId arm = doc.add_object(doc.make_object("Arm"), cube);
    app.sync().rebuild(engine, doc);
    tick(engine, 2);
    doc.select(cube);
    app.show_timeline();
    app.add_animator(cube);
    app.new_animation_clip("Reach");
    tick(engine, 2);
    doc.select(arm);
    app.set_animation_record(true);
    app.set_animation_time(10.0f / 30.0f);
    tick(engine, 2);

    // A "drag": several moves of the position only.
    auto& md = *app.animation_clip();
    const size_t undo0 = 0;
    (void)undo0;
    for (int i = 1; i <= 5; ++i) app.set_object_transform(arm, glm::vec3(0.1f * i, 0, 0), glm::vec3(0), glm::vec3(1));
    tick(engine, 2);
    expect(md.find_track("Arm", "position") && md.find_track("Arm", "position")->keys.size() == 1 &&
               std::abs(md.find_track("Arm", "position")->keys[0].value.x - 0.5f) < 1e-5f,
           "Record auto-keys the moved channel at the playhead (the last value of the drag)");
    expect(!md.find_track("Arm", "rotation_quat") && !md.find_track("Arm", "scale"), "...and only that channel");
    app.undo();
    tick(engine, 1);
    expect(!app.animation_clip()->find_track("Arm", "position"), "the whole drag is one undo step");
    app.redo();
    tick(engine, 1);

    // The inspector diamond keys one channel; it then reads as keyed on this frame.
    expect(!app.keyed_now(arm, "scale"), "scale is not keyed yet");
    app.key_channel(arm, "scale");
    tick(engine, 1);
    expect(app.keyed_now(arm, "scale") && app.animation_clip()->find_track("Arm", "scale"), "the diamond keys scale at this frame");

    // A second key, then Step interpolation holds the first value until the next key.
    app.set_animation_time(20.0f / 30.0f);
    tick(engine, 1);
    app.set_object_transform(arm, glm::vec3(1.5f, 0, 0), glm::vec3(0), glm::vec3(1));
    tick(engine, 1);
    app.select_all_keys();
    app.set_selected_key_interpolation("step");
    app.set_animation_time(15.0f / 30.0f);
    tick(engine, 3);
    const float x_mid = app.sync().live(arm)->get_transform()->transform().position().x;
    expect(std::abs(x_mid - 0.5f) < 1e-4f, "Step interpolation holds the earlier key (x = " + std::to_string(x_mid) + ")");
    const fs::path dir = doc.path().parent_path() / "animations" / "cube";
    const ClipModel on_disk = ClipModel::from_node(coopa::yaml::load_document(dir / "Reach.yaml"));
    expect(on_disk.find_track("Arm", "position") && on_disk.find_track("Arm", "position")->keys[0].easing == "step",
           "the interpolation is in the clip file");

    // Rename.
    app.rename_animation_clip("Grab");
    tick(engine, 2);
    const Node& anim = doc.find(cube)->at("components").as_seq()[static_cast<size_t>(doc.find_component(cube, "Animator"))];
    expect(fs::exists(dir / "Grab.yaml") && !fs::exists(dir / "Reach.yaml") && get_string(anim, "auto_play") == "Grab" &&
               get_string(anim.at("states").as_seq()[0], "clip") == "animations/cube/Grab.yaml",
           "renaming moves the file and updates the Animator's state and auto_play");
    dump(engine, "18_autokey");
}

/**
 * @brief The shipped rigs in the editor. In the animation_demo scene each rig's Timeline offers
 *        exactly that object's clips (from any object inside it) and previews them, leaving the
 *        clip files untouched. As object assets, the Objects tab lists them, opening one gives a
 *        working Timeline, and a copy placed in a scene plays its clips (paths resolve from the
 *        object asset to the shared assets/animations) -- and points back to the asset to edit.
 */
COOPA_TEST(rigs_preview_their_own_clips_and_play_when_placed) {
    EditorSession session({.prepare = [](Project& project) {
        copy_rig_assets(project.assets());   // the rig object assets, their clips and meshes
        const fs::path dst = project.assets() / "scenes" / "animation_demo";
        fs::create_directories(dst.parent_path());
        fs::copy(fs::path(ROOT_DIR) / "assets" / "scenes" / "animation" / "animation_demo", dst, fs::copy_options::recursive);
    }});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;
    const fs::path dst = project.assets() / "scenes" / "animation_demo";
    expect(app.open_scene(dst / "scene.yaml"), "the animation test scene opens");
    tick(engine, 4);
    app.show_timeline();
    auto& doc = app.document();
    auto states_of = [&](ObjectId rig) {
        std::vector<std::string> out;
        const Node& a = doc.find(rig)->at("components").as_seq()[static_cast<size_t>(doc.find_component(rig, "Animator"))];
        for (const auto& st : a.at("states").as_seq()) out.push_back(get_string(st, "name"));
        return out;
    };
    const ObjectId arm = object_named(app, "robot_arm"), elbow = object_named(app, "elbow"), ball = object_named(app, "bouncing_ball");
    expect(arm && elbow && ball, "the rigs are in the scene");
    doc.select(elbow);
    tick(engine, 2);
    expect(app.animation_rig() == arm && app.animation_clip() && app.animation_clip_state() == "wave",
           "selecting a joint opens its rig's first clip");
    expect(states_of(arm) == std::vector<std::string>{"wave", "idle"}, "the arm's clips are its own two");
    const glm::quat rest = app.sync().live(elbow)->get_transform()->transform().rotation_quat();
    app.set_animation_time(0.5f);
    tick(engine, 2);
    const glm::quat posed = app.sync().live(elbow)->get_transform()->transform().rotation_quat();
    expect(std::abs(glm::dot(rest, posed)) < 0.95f, "scrubbing previews the clip on the joint");
    dump(engine, "19_animation_test");
    doc.select(ball);
    tick(engine, 2);
    expect(app.animation_rig() == ball && app.animation_clip_state() == "bounce", "the ball's Timeline is the ball's own clip");
    app.set_timeline_rest_pose(true);
    tick(engine, 2);
    // Nothing was edited: every clip file is byte-identical to the shipped one.
    for (const char* rel : {"animations/robot_arm/wave.yaml", "animations/bouncing_ball/bounce.yaml"}) {
        const Node a = coopa::yaml::load_document(project.assets() / rel);
        const Node b = coopa::yaml::load_document(fs::path(ROOT_DIR) / "assets" / rel);
        expect(a == b, std::string("browsing and previewing leaves ") + rel + " untouched");
    }

    // The rigs as object assets.
    app.set_timeline_rest_pose(false);
    const auto objects = app.list_assets(AssetType::Object);
    auto listed = [&](const std::string& rel) { return std::find(objects.begin(), objects.end(), rel) != objects.end(); };
    expect(listed("objects/robot_arm.yaml") && listed("objects/tentacle.yaml") && listed("objects/bouncing_ball.yaml"),
           "the Objects tab lists the test rigs (" + std::to_string(objects.size()) + ")");

    // Open the robot arm: a rig with its clips, previewed by the Timeline.
    expect(app.open_object_asset(project.assets() / "objects" / "robot_arm.yaml"), "the robot arm object asset opens");
    tick(engine, 3);
    app.show_timeline();
    const ObjectId asset_elbow = object_named(app, "elbow");
    app.document().select(asset_elbow);
    tick(engine, 2);
    expect(app.animation_clip() && app.animation_clip_state() == "wave", "its Timeline opens the arm's clip (shared from assets/animations)");
    const glm::quat asset_rest = app.sync().live(asset_elbow)->get_transform()->transform().rotation_quat();
    app.set_animation_time(0.5f);
    tick(engine, 2);
    expect(std::abs(glm::dot(asset_rest, app.sync().live(asset_elbow)->get_transform()->transform().rotation_quat())) < 0.95f,
           "scrubbing poses the object asset's rig");
    dump(engine, "20_robot_arm_asset");

    // Place the ball in a scene and play: the placed copy bounces from the shared clip.
    app.show_document_view();
    app.open_scene(project.assets() / "scenes" / "main" / "scene.yaml");
    tick(engine, 3);
    const ObjectId placed = app.place_object_asset("objects/bouncing_ball.yaml", glm::vec3(2, 0, 0));
    tick(engine, 3);
    app.document().select(placed);
    tick(engine, 2);
    expect(app.animation_instance_asset(placed) == "objects/bouncing_ball" && app.animation_rig() == 0,
           "a placed copy points the Timeline at its object asset");
    app.play();
    tick(engine, 15);
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 60; ++i) {
        tick(engine, 1);
        coopa::scene::SceneObject* ball = nullptr;
        for (auto* o : engine.scene().get_components<coopa::gfx::engine::components::MeshRenderer>()) {
            if (o->owner && o->owner->name() == "ball") ball = o->owner;
        }
        if (!ball) continue;
        const float z = glm::vec3(ball->get_transform()->get_world_matrix()[3]).z;
        lo = std::min(lo, z);
        hi = std::max(hi, z);
    }
    expect(app.playing() && hi - lo > 1.0f, "in Play the placed ball bounces (z " + std::to_string(lo) + " .. " + std::to_string(hi) + ")");
    app.stop();
    tick(engine, 2);
}

} // namespace toy::editor::testing
