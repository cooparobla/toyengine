/**
 * @file origin_glyph_test.cpp
 * @brief Object origin glyphs: the per-object Origin Style and Size reach the picture.
 *
 * These are what a rig's joints draw -- one glyph per bone object, which is most of what is on
 * screen in a character scene. Both settings live on the object's Transform, are inherited from
 * the nearest ancestor that sets one, and are read at draw time -- so asserting on the stored
 * value proves nothing; each check here captures the editor's frame and compares it.
 */

#include <coopa/testing/test.h>

#include <gfxcoopa/util/image_readback.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("origin_glyph");

namespace toy::editor::testing {
namespace {

size_t changed_pixels(const coopa::gfx::util::ImageData& a, const coopa::gfx::util::ImageData& b) {
    size_t n = 0;
    for (size_t i = 0; i + 2 < a.pixels.size() && i + 2 < b.pixels.size(); i += a.channels) {
        const int d = std::abs(int(a.pixels[i]) - int(b.pixels[i])) +
                      std::abs(int(a.pixels[i + 1]) - int(b.pixels[i + 1])) +
                      std::abs(int(a.pixels[i + 2]) - int(b.pixels[i + 2]));
        if (d > 12) ++n;
    }
    return n;
}

} // namespace

COOPA_TEST(origin_size_and_style_change_the_picture) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    // A little chain of mesh-less objects, as a rig's joints are.
    const ObjectId cube = object_named(app, "cube");
    const ObjectId hip = doc.add_object(doc.make_object("hip"), cube);
    const ObjectId knee = doc.add_object(doc.make_object("knee"), hip);
    doc.add_object(doc.make_object("ankle"), knee);
    doc.set_transform(hip, {0.0f, 0.0f, 1.0f}, {}, {1, 1, 1}, "pose");
    doc.set_transform(knee, {0.0f, 0.0f, 1.0f}, {}, {1, 1, 1}, "pose");
    app.sync().rebuild(engine, doc);
    doc.clear_selection();
    tick(engine, 4);

    // Set on the root cube; every joint under it inherits, which is the point of the feature.
    auto frame = [&](const std::string& style, float size) {
        doc.select(cube);
        static const std::vector<std::string> names = {"Axes", "Orb"};
        for (size_t k = 0; k < names.size(); ++k) {
            if (names[k] == style) app.set_selection_origin_style(static_cast<OriginStyle>(k));
        }
        app.set_selection_origin_size(size);
        doc.clear_selection();
        tick(engine, 4);
        dump(engine, "origin_" + style + "_" + std::to_string(int(size * 1000)));
        return engine.capture_image(false);
    };

    const coopa::gfx::util::ImageData small = frame("Axes", 0.08f);
    const coopa::gfx::util::ImageData big = frame("Axes", 0.5f);
    const size_t by_size = changed_pixels(small, big);
    expect(by_size > 150, "Origin Size draws bigger origins (" + std::to_string(by_size) + " pixels moved)");
    expect(app.origin_size_for(hip) == 0.5f, "...and a joint under it inherits that size");

    const coopa::gfx::util::ImageData axes = frame("Axes", 0.5f);
    const coopa::gfx::util::ImageData orb = frame("Orb", 0.5f);
    expect(changed_pixels(axes, orb) > 150, "Orb looks nothing like Axes (" +
                                                std::to_string(changed_pixels(axes, orb)) + " pixels moved)");
    expect(app.origin_style_for(hip) == OriginStyle::Orb && app.origin_style_for(knee) == OriginStyle::Orb,
           "a style set on the root reaches every joint under it");

}

/**
 * @brief The Bone style aims at the real joints: one bone per child, pointing at it, so a rig
 *        reads as a skeleton. A leaf carries on from its parent; an explicit length overrides.
 */
COOPA_TEST(bones_point_at_the_child_joints) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    const ObjectId cube = object_named(app, "cube");
    // A hip that splits into two legs, one of them carrying on to a foot.
    const ObjectId hip = doc.add_object(doc.make_object("hip"), cube);
    const ObjectId left = doc.add_object(doc.make_object("left_knee"), hip);
    const ObjectId right = doc.add_object(doc.make_object("right_knee"), hip);
    const ObjectId foot = doc.add_object(doc.make_object("left_foot"), left);
    doc.set_transform(hip, {0.0f, 0.0f, 1.0f}, {}, {1, 1, 1}, "pose");
    doc.set_transform(left, {-0.4f, 0.0f, -0.5f}, {}, {1, 1, 1}, "pose");
    doc.set_transform(right, {0.4f, 0.0f, -0.5f}, {}, {1, 1, 1}, "pose");
    doc.set_transform(foot, {0.0f, 0.0f, -0.5f}, {}, {1, 1, 1}, "pose");
    app.sync().rebuild(engine, doc);
    tick(engine, 3);

    auto world_of = [&](ObjectId id) {
        auto* live = app.sync().live(id);
        return glm::vec3(live->get_transform()->transform().get_world_matrix()[3]);
    };
    const glm::vec3 hip_p = world_of(hip), left_p = world_of(left), right_p = world_of(right);

    // A joint with two children draws a bone to each, landing exactly on them.
    const std::vector<glm::vec3> hip_tails = app.bone_tails_for(hip);
    expect(hip_tails.size() == 2, "the hip draws one bone per leg (" + std::to_string(hip_tails.size()) + ")");
    auto reaches = [&](const std::vector<glm::vec3>& tails, const glm::vec3& joint) {
        for (const glm::vec3& t : tails) if (glm::distance(t, joint) < 1e-4f) return true;
        return false;
    };
    expect(reaches(hip_tails, left_p) && reaches(hip_tails, right_p), "...and each lands on its knee");

    // A single-child joint reaches that child, whatever way its own axes face.
    const std::vector<glm::vec3> left_tails = app.bone_tails_for(left);
    expect(left_tails.size() == 1 && glm::distance(left_tails[0], world_of(foot)) < 1e-4f,
           "a one-child joint reaches that joint");

    // A leaf carries on the way its parent points, half as far -- not off along some axis.
    const std::vector<glm::vec3> foot_tails = app.bone_tails_for(foot);
    expect(foot_tails.size() == 1, "a leaf still draws a bone");
    const glm::vec3 foot_p = world_of(foot);
    const glm::vec3 incoming = glm::normalize(foot_p - left_p);
    const glm::vec3 outgoing = glm::normalize(foot_tails[0] - foot_p);
    expect(glm::dot(incoming, outgoing) > 0.999f, "...carrying on the way its parent points");
    expect(std::abs(glm::distance(foot_tails[0], foot_p) - glm::distance(foot_p, left_p) * 0.5f) < 1e-4f,
           "...for half its parent's span");

    // A prop or a light parented to a joint is not a joint: no bone runs out to it.
    const ObjectId prop = doc.add_object(doc.make_object("held_prop"), left);
    doc.set_transform(prop, {0.6f, 0.0f, 0.0f}, {}, {1, 1, 1}, "pose");
    Node mr = Node::mapping();
    mr["type"] = Node(std::string("MeshRenderer"));
    mr["mesh_path"] = Node(std::string("cube"));
    doc.find(prop)->operator[]("components").as_seq().push_back(mr);
    const ObjectId lamp = doc.add_object(doc.make_object("hand_lamp"), left);
    doc.set_transform(lamp, {0.0f, 0.6f, 0.0f}, {}, {1, 1, 1}, "pose");
    Node pl = Node::mapping();
    pl["type"] = Node(std::string("PointLight"));
    doc.find(lamp)->operator[]("components").as_seq().push_back(pl);
    app.sync().rebuild(engine, doc);
    tick(engine, 3);
    const std::vector<glm::vec3> with_props = app.bone_tails_for(left);
    expect(with_props.size() == 1 && glm::distance(with_props[0], world_of(foot)) < 1e-4f,
           "a mesh prop and a light on a joint add no bones (" + std::to_string(with_props.size()) + " tail(s))");

    // An explicit bone_length collapses the hip to one bone of exactly that length.
    const int ci = doc.find_component(hip, "Transform");
    Node t = doc.find(hip)->at("components").as_seq()[static_cast<size_t>(ci)];
    t["bone_length"] = make_float(0.25);
    doc.set_component(hip, ci, t, "Bone Length");
    const std::vector<glm::vec3> forced = app.bone_tails_for(hip);
    expect(forced.size() == 1 && std::abs(glm::distance(forced[0], hip_p) - 0.25f) < 1e-4f,
           "a stored bone_length draws one bone of that length");

    // A look at the rig itself (EDITOR_DUMP_DIR), in each style.
    t.as_map().erase("bone_length");
    doc.set_component(hip, ci, t, "Bone Length");
    app.camera().focus = hip_p;
    app.camera().distance = 3.0f;
    app.camera().apply();
    for (int k = 0; k < 2; ++k) {
        doc.select(hip);
        app.set_selection_origin_style(static_cast<OriginStyle>(k));
        doc.clear_selection();
        tick(engine, 4);
        dump(engine, std::string("rig_") + std::to_string(k));
    }
}

/**
 * @brief The settings live on the object: an object with none takes the nearest ancestor's,
 *        Axes when nobody sets one, and a child's own value wins over what it would inherit.
 */
COOPA_TEST(the_style_is_per_object_and_inherited) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    const ObjectId cube = object_named(app, "cube");
    const ObjectId root = doc.add_object(doc.make_object("rig"), cube);
    const ObjectId hip = doc.add_object(doc.make_object("hip"), root);
    const ObjectId knee = doc.add_object(doc.make_object("knee"), hip);
    doc.set_transform(hip, {0.0f, 0.0f, 1.0f}, {}, {1, 1, 1}, "pose");
    doc.set_transform(knee, {0.0f, 0.0f, 1.0f}, {}, {1, 1, 1}, "pose");
    app.sync().rebuild(engine, doc);
    tick(engine, 3);

    expect(app.origin_style_for(knee) == OriginStyle::Axes, "nothing set anywhere: Axes is the default");
    expect(app.origin_size_for(knee) == kDefaultOriginSize, "...at the default size");

    // The viewer-wide default is the bottom of the chain, and any object outranks it.
    expect(app.set_preference("origin_style", Node(std::string("Orb"))), "origin_style is still a preference");
    app.set_preference("origin_size", make_float(0.1));
    expect(app.default_origin_style() == OriginStyle::Orb, "the preference sets the default");
    expect(app.origin_style_for(knee) == OriginStyle::Orb, "...and an object with nothing set follows it");
    expect(std::abs(app.origin_size_for(knee) - 0.1f) < 1e-4f, "...size too");
    expect(get_string(Project::load_prefs(), "origin_style") == "Orb", "...saved by name");
    app.set_preference("origin_style", Node(std::string("Axes")));
    expect(app.origin_style_for(knee) == OriginStyle::Axes, "...and follows it back");

    // Set the rig's root; everything under it follows.
    doc.select(root);
    app.set_selection_origin_style(OriginStyle::Orb);
    app.set_selection_origin_size(0.2f);
    expect(app.origin_style_for(hip) == OriginStyle::Orb && app.origin_style_for(knee) == OriginStyle::Orb,
           "a style on the rig's root reaches every joint under it");
    expect(std::abs(app.origin_size_for(knee) - 0.2f) < 1e-4f, "...and so does the size");
    expect(app.origin_style_for(cube) == OriginStyle::Axes, "...without touching anything above it, which keeps the default");

    // A child's own value wins.
    doc.select(knee);
    app.set_selection_origin_style(OriginStyle::Axes);
    expect(app.origin_style_for(knee) == OriginStyle::Axes, "an object's own style beats what it would inherit");
    expect(app.origin_style_for(hip) == OriginStyle::Orb, "...and leaves its siblings alone");

    // Back to Inherit, and it follows the root again.
    app.set_selection_origin_style(std::nullopt);
    expect(app.origin_style_for(knee) == OriginStyle::Orb, "Inherit hands it back to the ancestor");
    // ...and with the ancestor cleared too, all the way back to the preference.
    doc.select(root);
    app.set_selection_origin_style(std::nullopt);
    expect(app.origin_style_for(knee) == OriginStyle::Axes, "clearing the ancestor falls back to the default");

    doc.select(root);
    app.set_selection_origin_style(OriginStyle::Orb);
    // One undo step per edit, across the whole selection.
    doc.select(hip);
    doc.select(knee, true);
    app.set_selection_origin_style(OriginStyle::Axes);
    expect(app.origin_style_for(hip) == OriginStyle::Axes && app.origin_style_for(knee) == OriginStyle::Axes,
           "the setting applies to every selected object");
    app.undo();
    expect(app.origin_style_for(hip) == OriginStyle::Orb && app.origin_style_for(knee) == OriginStyle::Orb,
           "...and one undo takes the whole selection back");
    app.set_preference("origin_style", Node(std::string("Axes")));
    app.set_preference("origin_size", make_float(kDefaultOriginSize));
}

/**
 * @brief Each style is clickable as drawn: the middle of a bone's body, a point out along an
 *        axis line, the ball of an orb -- not just a dot on the origin.
 */
COOPA_TEST(every_style_is_clickable_where_it_is_drawn) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    const ObjectId cube = object_named(app, "cube");
    const ObjectId hip = doc.add_object(doc.make_object("hip"), cube);
    const ObjectId knee = doc.add_object(doc.make_object("knee"), hip);
    doc.set_transform(hip, {0.0f, 0.0f, 1.2f}, {}, {1, 1, 1}, "pose");
    doc.set_transform(knee, {0.0f, 0.0f, 1.2f}, {}, {1, 1, 1}, "pose");
    app.sync().rebuild(engine, doc);
    app.camera().focus = {0.0f, 0.0f, 1.8f};
    app.camera().distance = 6.0f;
    app.camera().apply();
    doc.clear_selection();
    tick(engine, 4);

    auto& in = session.in;
    // The hip sits at the cube's top, the knee 1.2 m above it: offsets are world offsets here,
    // since neither joint is rotated or scaled.
    const auto hip_px = screen_of(engine, app, hip, glm::vec3(0.0f), in.scale);
    const auto mid = screen_of(engine, app, hip, {0.0f, 0.0f, 0.6f}, in.scale);      // the bone's middle
    const auto along = screen_of(engine, app, hip, {0.0f, 0.0f, 0.32f}, in.scale);   // out along the Z axis line
    coopa::test::require(hip_px && mid && along, "the hip and its glyph are on screen");

    auto set_style = [&](OriginStyle st, float size) {
        doc.select(cube);
        app.set_selection_origin_style(st);
        app.set_selection_origin_size(size);
        doc.clear_selection();
        tick(engine, 3);
    };

    // Axes: out along the Z line, off the origin.
    set_style(OriginStyle::Axes, 0.4f);
    expect(glm::distance(*along, *hip_px) > 10.0f, "the click is off the origin dot");
    expect(app.pick_object(*along) == hip, "clicking along an axis line selects it");

    // Orb: the ball, and nothing much beyond it.
    set_style(OriginStyle::Orb, 0.3f);
    expect(app.pick_object(*hip_px) == hip, "clicking an orb selects it");
    expect(app.pick_object(*hip_px + glm::vec2(40.0f, 0.0f)) != hip, "...and empty space next to it does not");

    // A glyph is only clickable while it is drawn.
    set_style(OriginStyle::Axes, 0.4f);
    app.set_show_origins(false);
    tick(engine, 3);
    expect(app.pick_object(*along) != hip, "with Origins off, the axes take no clicks");
    app.set_show_origins(true);
}

/**
 * @brief Clicking the same spot twice steps past what is selected, so a joint buried in a mesh
 *        is reachable: first click the chest, second click the joint inside it (Blender).
 */
COOPA_TEST(a_repeat_click_steps_past_the_selection) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    auto& in = session.in;
    // A joint chain running up through the cube, so the bones are inside the mesh.
    const ObjectId cube = object_named(app, "cube");
    const ObjectId hip = doc.add_object(doc.make_object("hip"), cube);
    const ObjectId knee = doc.add_object(doc.make_object("knee"), hip);
    doc.set_transform(hip, {0.0f, 0.0f, -0.3f}, {}, {1, 1, 1}, "pose");
    doc.set_transform(knee, {0.0f, 0.0f, 0.6f}, {}, {1, 1, 1}, "pose");
    app.sync().rebuild(engine, doc);
    doc.select(cube);
    app.set_selection_origin_style(OriginStyle::Axes);
    app.set_selection_origin_size(0.3f);
    doc.clear_selection();
    tick(engine, 4);

    // Aim along the hip's Z axis line, which runs inside the cube.
    const auto at = screen_of(engine, app, hip, {0.0f, 0.0f, 0.2f}, in.scale);
    coopa::test::require(at.has_value(), "the hip's axis line is on screen");
    const std::vector<ObjectId> under = app.pick_candidates_for_test(*at);
    expect(std::find(under.begin(), under.end(), cube) != under.end() &&
               std::find(under.begin(), under.end(), hip) != under.end(),
           "both the mesh and the joint are under the cursor (" + std::to_string(under.size()) + " candidates)");
    expect(under.front() == cube, "the mesh surface is in front, so it is picked first");

    // Click once: the mesh. Click the same spot again: the bone behind it.
    in.click(*at);
    tick(engine, 2);
    expect(doc.primary() == cube, "the first click selects the chest mesh");
    in.click(*at);
    tick(engine, 2);
    const ObjectId second = doc.primary();
    expect(second != cube, "the second click in the same place steps past the mesh it just selected");
    expect(std::find(under.begin(), under.end(), second) != under.end(), "...onto something else under the cursor");
    // Keep clicking and the cycle reaches every joint buried in the mesh, the hip among them.
    bool reached_hip = second == hip;
    for (int i = 0; i < 4 && !reached_hip; ++i) {
        in.click(*at);
        tick(engine, 2);
        reached_hip = doc.primary() == hip;
    }
    expect(reached_hip, "clicking on walks the cycle to the joint inside the mesh");

    // Clicking somewhere else starts over rather than carrying on cycling. Well clear of the
    // 4 px that counts as the same spot.
    const glm::vec2 away = *at + glm::vec2(0.0f, 45.0f);
    coopa::test::require(glm::distance(away, *at) > 10.0f, "the other click is nowhere near the first");
    in.click(away);
    tick(engine, 2);
    in.click(*at);
    tick(engine, 2);
    expect(doc.primary() == cube, "a click in a new place picks the front thing again, not the next in the cycle");
}

/**
 * @brief Pose mode draws the rig as bones on its own: no origin style offers them, they show
 *        whatever Origins is set to, and they are clickable like any other glyph.
 */
COOPA_TEST(pose_mode_draws_and_picks_bones) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    auto& in = session.in;
    const ObjectId cube = object_named(app, "cube");
    const ObjectId hip = doc.add_object(doc.make_object("hip"), cube);
    doc.add_object(doc.make_object("knee"), hip);
    doc.set_transform(hip, {0.0f, 0.0f, 1.2f}, {}, {1, 1, 1}, "pose");
    doc.set_transform(object_named(app, "knee"), {0.0f, 0.0f, 1.2f}, {}, {1, 1, 1}, "pose");
    app.sync().rebuild(engine, doc);
    app.camera().focus = {0.0f, 0.0f, 1.8f};
    app.camera().distance = 6.0f;
    app.camera().apply();
    doc.select(cube);
    app.show_timeline();
    app.add_animator(cube);
    doc.clear_selection();
    tick(engine, 4);
    const coopa::gfx::util::ImageData before = engine.capture_image(false);

    const auto mid = screen_of(engine, app, hip, {0.0f, 0.0f, 0.6f}, in.scale);
    coopa::test::require(mid.has_value(), "the bone's middle would be on screen");
    expect(app.pick_object(*mid) != hip, "outside Pose mode nothing is drawn there to click");

    doc.select(cube);
    expect(app.set_interaction_mode(InteractionMode::Pose), "Pose Mode opens on the rig");
    doc.clear_selection();
    tick(engine, 4);
    const coopa::gfx::util::ImageData posed = engine.capture_image(false);
    expect(changed_pixels(before, posed) > 150, "Pose Mode draws the rig's bones (" +
                                                    std::to_string(changed_pixels(before, posed)) + " pixels moved)");
    expect(app.pick_object(*mid) == hip, "...and the middle of a bone selects that joint");

    // They are the mode's own display, not the Origins overlay.
    app.set_show_origins(false);
    tick(engine, 3);
    expect(app.pick_object(*mid) == hip, "turning Origins off does not take the bones with it");
    app.set_show_origins(true);

    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 4);
    const coopa::gfx::util::ImageData after = engine.capture_image(false);
    expect(changed_pixels(posed, after) > 150, "leaving Pose Mode takes the bones away again (" +
                                                   std::to_string(changed_pixels(posed, after)) + " pixels moved)");
}

/** @brief "Origins" shows and hides them; "Cameras / Lights" is a separate toggle. */
COOPA_TEST(the_origins_toggle_is_what_hides_them) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();
    const ObjectId cube = object_named(app, "cube");
    doc.add_object(doc.make_object("joint"), cube);
    doc.set_transform(object_named(app, "joint"), {0.0f, 0.0f, 1.0f}, {}, {1, 1, 1}, "pose");
    app.sync().rebuild(engine, doc);
    doc.clear_selection();
    tick(engine, 4);

    auto shot = [&](bool origins, bool glyphs) {
        app.set_show_origins(origins);
        app.set_show_glyphs(glyphs);
        tick(engine, 4);
        return engine.capture_image(false);
    };
    const coopa::gfx::util::ImageData both = shot(true, true);
    const coopa::gfx::util::ImageData no_origins = shot(false, true);
    expect(changed_pixels(both, no_origins) > 100, "turning Origins off hides the origin glyphs (" +
                                                       std::to_string(changed_pixels(both, no_origins)) + " pixels moved)");
    // With the camera and light symbols off, the origins must still be there -- turning them
    // off on top of that has to change the picture again.
    const coopa::gfx::util::ImageData no_glyphs = shot(true, false);
    const coopa::gfx::util::ImageData neither = shot(false, false);
    expect(changed_pixels(no_glyphs, neither) > 100,
           "Cameras / Lights does not take the origins with it (" +
               std::to_string(changed_pixels(no_glyphs, neither)) + " pixels moved)");
}

} // namespace toy::editor::testing
