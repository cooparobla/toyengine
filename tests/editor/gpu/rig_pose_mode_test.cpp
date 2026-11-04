/**
 * @file rig_pose_mode_test.cpp
 * @brief Pose Mode on a coopa::rig::Rig character (the mannequin prefab) through real input:
 *        entering Pose Mode resolves past the old Animator-only gate, a click on a bone selects
 *        it, G/R/S with a typed value poses it in world space, and Alt+G clears it back to rest.
 */

#include <coopa/testing/test.h>

#include <coopa/rig/rig_component.h>
#include <coopa/rig/skeletal_clip_loader.h>
#include <coopa/yaml/document.h>

#include "editor/anim/bone_clip_model.h"
#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("rig_pose_mode");

namespace toy::editor::testing {

using namespace coopa::test;

namespace {

coopa::rig::Rig* mannequin_rig(EditorApp& app, ObjectId id) {
    auto* live = app.sync().live(id);
    return live ? live->get_component<coopa::rig::Rig>() : nullptr;
}

/** @brief A point comfortably inside a bone's own body -- its visible mid-section -- rather than
 *         right at its head, which for a "connected" bone coincides with the parent's tail and so
 *         is the single worst (zero-width, ambiguous) point on screen to click. */
glm::vec3 bone_mid_world(coopa::rig::Rig& rig, int bone) {
    const glm::mat4 w = rig.world_matrix(static_cast<std::size_t>(bone));
    const glm::vec3 head(w[3]);
    const glm::vec3 tail(w * glm::vec4(0.0f, rig.bone_length(static_cast<std::size_t>(bone)), 0.0f, 1.0f));
    return glm::mix(head, tail, 0.5f);
}

} // namespace

/** @brief Pose Mode opens on a coopa::rig::Rig character (no Animator anywhere above it) --
 *         the gate this whole mode was blocked behind before this session's Stage 6 work. */
COOPA_TEST(pose_mode_opens_on_a_coopa_rig_character) {
    using coopa::input::Key;
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    Node mannequin = doc.make_object("mannequin");
    mannequin["prefab"] = Node(std::string("objects/characters/mannequin"));
    const ObjectId id = doc.add_object(mannequin);
    app.sync().rebuild(engine, doc);
    tick(engine, 4);

    coopa::rig::Rig* rig = mannequin_rig(app, id);
    coopa::test::require(rig != nullptr, "the mannequin instance resolved a Rig component");
    coopa::test::require(rig->is_loaded(), "its RigAsset finished loading");
    const int pelvis = rig->find_bone("pelvis");
    coopa::test::require(pelvis >= 0, "the rig has a 'pelvis' bone");

    doc.select(id);
    expect(app.set_interaction_mode(InteractionMode::Pose),
           "Pose Mode opens on a coopa::rig::Rig object directly -- no Animator needed above it");
    expect(app.interaction_mode() == InteractionMode::Pose, "...and reports Pose Mode");
    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief A click on a bone in Pose Mode, then G X <value> Enter, poses that bone -- moving its
 *         resolved WORLD head by exactly the typed delta along world X. */
COOPA_TEST(click_and_grab_pose_a_bone_in_world_space) {
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

    const glm::vec3 before(rig->world_matrix(static_cast<std::size_t>(chest))[3]);
    glm::vec2 px;
    coopa::test::require(engine.world_to_window(bone_mid_world(*rig, chest), px), "chest's body projects into the viewport");
    in.click(px / in.scale);

    in.key(Key::G);
    in.key(Key::X);
    in.key(Key::Num2);
    in.key(Key::Enter);
    tick(engine, 2);

    const glm::vec3 after(rig->world_matrix(static_cast<std::size_t>(chest))[3]);
    expect_near(after.x - before.x, 2.0f, 0.01f,
               "G X 2 Enter on the picked chest bone moves its world head exactly 2 m along world X (got " +
                   std::to_string(after.x - before.x) + ")");
    expect_near(after.y - before.y, 0.0f, 0.01f, "...and nothing on Y");
    expect_near(after.z - before.z, 0.0f, 0.01f, "...and nothing on Z");

    // Alt+G clears the pose location back to rest.
    in.key(Key::G, coopa::input::Mods::Alt);
    tick(engine, 2);
    const glm::vec3 cleared(rig->world_matrix(static_cast<std::size_t>(chest))[3]);
    expect(glm::distance(cleared, before) < 0.01f, "Alt+G clears the bone's pose location back to rest (" +
                                                        std::to_string(glm::distance(cleared, before)) + " m off)");
    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief Ctrl+C/Ctrl+V round-trips a bone's own pose by name (copy, clear, paste restores it),
 *         and Ctrl+M mirrors a posed arm onto its `_r` counterpart without flipping which way
 *         it bends. */
COOPA_TEST(copy_paste_and_mirror_bone_pose) {
    using coopa::input::Key;
    using coopa::input::Mods;
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
    const int arm_l = rig->find_bone("upper_arm_l");
    const int arm_r = rig->find_bone("upper_arm_r");
    coopa::test::require(arm_l >= 0 && arm_r >= 0, "the rig has upper_arm_l and upper_arm_r bones");

    doc.select(id);
    coopa::test::require(app.set_interaction_mode(InteractionMode::Pose), "Pose Mode opens");

    glm::vec2 px;
    coopa::test::require(engine.world_to_window(bone_mid_world(*rig, arm_l), px), "upper_arm_l's body projects into the viewport");
    in.click(px / in.scale);
    in.key(Key::R);
    in.key(Key::Z);
    in.key(Key::Num3);
    in.key(Key::Num0);
    in.key(Key::Enter);
    tick(engine, 2);
    const glm::quat posed = rig->pose(static_cast<std::size_t>(arm_l)).rotation;
    expect(glm::dot(posed, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) < 0.999f, "R Z 30 actually changed arm_l's pose rotation");

    in.key(Key::C, Mods::Control);   // copy arm_l's pose (still the only selected bone)
    in.key(Key::R, Mods::Alt);       // clear its rotation back to rest
    expect(glm::all(glm::epsilonEqual(rig->pose(static_cast<std::size_t>(arm_l)).rotation, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 1e-5f)),
           "Alt+R cleared arm_l's pose rotation before the paste");
    in.key(Key::V, Mods::Control);   // paste restores what Ctrl+C copied, onto the same-named bone
    tick(engine, 2);
    expect(glm::all(glm::epsilonEqual(rig->pose(static_cast<std::size_t>(arm_l)).rotation, posed, 1e-5f)),
           "Ctrl+C then Alt+R then Ctrl+V restores arm_l's exact pose rotation by name");

    // Mirror the still-posed arm_l onto arm_r: expect exactly the swing-preserving mirror
    // bone_pose_edit_test.cpp already proves at the unit level (negate the rotation's Y/Z).
    const glm::quat r_before = rig->pose(static_cast<std::size_t>(arm_r)).rotation;
    in.key(Key::M, Mods::Control);
    tick(engine, 2);
    const glm::quat r_after = rig->pose(static_cast<std::size_t>(arm_r)).rotation;
    const glm::quat expected_mirror(posed.w, posed.x, -posed.y, -posed.z);
    expect(std::abs(glm::dot(r_before, r_after)) < 0.999f, "Ctrl+M changed arm_r's pose rotation");
    expect(std::abs(glm::dot(expected_mirror, r_after)) > 0.999f,
           "...to exactly arm_l's rotation mirrored across the rig's YZ plane (same swing, not the opposite)");
    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief I keys the selected bone's current pose into the SkeletalAnimator's open state's
 *         clip file (a scratch copy under the test project, never the real shipped asset) --
 *         saved immediately, and readable back through the exact runtime parser. */
COOPA_TEST(insert_key_writes_the_bones_pose_into_the_open_states_clip) {
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

    glm::vec2 px;
    coopa::test::require(engine.world_to_window(bone_mid_world(*rig, chest), px), "chest's body projects into the viewport");
    in.click(px / in.scale);
    in.key(Key::G);
    in.key(Key::X);
    in.key(Key::Num1);
    in.key(Key::Enter);
    tick(engine, 2);
    const glm::vec3 posed_location = rig->pose(static_cast<std::size_t>(chest)).location;

    in.key(Key::I);
    tick(engine, 2);

    // A brand-new clip with nowhere else to go is authored scene-locally (assets/scenes/<scene>/...),
    // the same convention the create-animation skill documents for a scene-only clip.
    const fs::path saved = doc.path().parent_path() / "animations" / "mannequin" / "idle.yaml";
    coopa::test::require(coopa::yaml::document_exists(saved),
                        "I saved a scratch idle.yaml under the test project's scene folder (never the real shipped asset): " +
                            saved.string());
    const BoneClipModel reloaded = BoneClipModel::from_node(coopa::yaml::load_document(saved));
    const BoneClipTrack* chest_track = reloaded.find_bone_track("chest");
    coopa::test::require(chest_track && !chest_track->location.keys.empty(), "the saved clip has a 'chest' location key");
    expect(glm::all(glm::epsilonEqual(glm::vec3(chest_track->location.keys[0].value), posed_location, 1e-4f)),
           "...matching exactly the pose that was on screen when I was pressed");

    // The runtime's own parser reads the same file the same way.
    const coopa::rig::SkeletalClip rt = coopa::rig::parse_skeletal_clip(coopa::yaml::load_document(saved));
    bool rt_found = false;
    for (const auto& t : rt.bones) if (t.bone == "chest") rt_found = !t.location.keys().empty();
    expect(rt_found, "...and the runtime's own skeletal clip parser reads the same key");
    app.set_interaction_mode(InteractionMode::Object);
}

} // namespace toy::editor::testing
