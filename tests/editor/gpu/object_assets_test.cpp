/**
 * @file object_assets_test.cpp
 * @brief Object assets (prefabs): create from a selection, place, override per instance, edit the asset
 * as a one-object document, spawn at runtime; instance overrides save as the smallest diff and
 * revert / apply; an opened asset's meshes pick and enter Edit Mode; a duplicated tile set owns
 * copies of its piece meshes.
 */

#include <coopa/testing/test.h>

#include "editor/support/editor_session.h"
#include <toyengine/scene/skinned_mesh_renderer.h>

COOPA_TEST_SUITE("object_assets");

namespace toy::editor::testing {

/** @brief Object assets: create from a selection, place instances, edit the asset, override, spawn. */
COOPA_TEST(create_place_override_edit_and_spawn) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;
    const fs::path root = session.project.root();
    const ObjectId cube = object_named(app, "cube");
    app.document().select(cube);
    expect(app.create_object_asset_from_selection(), "Create Object Asset from the selection");
    tick(engine, 3);
    const fs::path asset = project.assets() / "objects" / "cube.yaml";
    expect(coopa::yaml::document_exists(asset), "objects/cube.yaml is written");
    const Node* inst = app.document().find(cube);
    expect(inst && get_string(*inst, "prefab") == "objects/cube", "the selection becomes an instance (`prefab:`)");
    auto* live = engine.scene().find_object("cube");
    auto* mr = live ? live->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(mr && mr->is_ready(), "the instance still renders its mesh");
    const ObjectId second = app.place_object_asset("objects/cube.yaml", glm::vec3(3, 0, 0.5f));
    tick(engine, 3);
    expect(second != 0 && engine.scene().find_object(get_string(*app.document().find(second), "name")), "a second instance is placed");

    // An override on one instance: its own albedo, the other keeps the asset's.
    Node ov = Node::mapping();
    ov["type"] = Node(std::string("MeshRenderer"));
    Node mat = Node::mapping();
    mat["albedo"] = make_color({1.0f, 0.0f, 0.0f});
    ov["material"] = mat;
    app.document().add_component(second, ov);
    app.sync().rebuild(engine, app.document());
    tick(engine, 3);
    auto* l2 = engine.scene().find_object(get_string(*app.document().find(second), "name"));
    auto* m2 = l2 ? l2->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(m2 && m2->material.albedo.r > 0.99f && m2->is_ready(), "an instance override changes only that instance (mesh still from the asset)");
    expect(app.save_scene(), "the scene with instances saves");

    // The object asset opens as its own document; the hierarchy root is the object.
    app.open_asset(AssetType::Object, "objects/cube.yaml");
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::Object && app.document().is_object_asset() && app.document().object_root() != 0,
           "an object asset opens as a one-object document");
    const ObjectId oroot = app.document().object_root();
    const ObjectId child = app.create_empty();
    tick(engine, 2);
    expect(child && app.document().parent_of(child).value_or(0) == oroot, "objects added to an object asset become children of its root");
    expect(app.save_scene(), "the object asset saves");
    const Node saved = coopa::yaml::load_document(asset);
    expect(saved.contains("object") && !saved.contains("scene") && saved.at("object").contains("children"),
           "it is written back as `object:` with the new child");

    // Runtime: the engine instantiates object assets.
    app.open_asset(AssetType::Scene, "scenes/main/scene.yaml");
    tick(engine, 4);
    const size_t before = engine.scene().root_objects().size();
    auto* spawned = engine.spawn("objects/cube", glm::vec3(0, 3, 1));
    expect(spawned && engine.scene().root_objects().size() == before + 1 && spawned->children().size() == 1,
           "Engine::spawn() instantiates the asset, child included");
    dump(engine, "17_object_instances");
}

/**
 * @brief Overrides on an instance's inherited parts: they get nodes of their own, a click picks
 *        the instance then the part, edits save as the smallest override (never touching the
 *        asset), Revert / Apply to Object Asset, and renaming the part in the asset follows.
 */
COOPA_TEST(instance_overrides_save_minimally_revert_and_apply) {
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;
    auto& doc = app.document();
    auto name_of = [&](ObjectId id) { return id && doc.find(id) ? get_string(*doc.find(id), "name") : std::string("nothing"); };

    // An object asset whose mesh sits on a child: an empty with a raised sphere under it.
    const ObjectId lamp = app.create_empty();
    tick(engine, 2);
    const ObjectId shade = app.create_primitive("Sphere", lamp);
    tick(engine, 2);
    const std::string part_name = name_of(shade);
    doc.set_transform(shade, {0, 0, 1.5f}, {0, 0, 0}, glm::vec3(1), "Move");
    doc.select(lamp);
    expect(app.create_object_asset_from_selection(), "the empty and its sphere become an object asset");
    tick(engine, 4);
    expect(doc.is_instance(lamp), "...and an instance takes their place");
    const std::string ref = get_string(*doc.find(lamp), "prefab");
    const fs::path asset_file = project.assets() / (ref + ".yaml");
    const std::string asset_before = coopa::yaml::emit(coopa::yaml::load_document(asset_file));

    auto part_of = [&](ObjectId inst, const std::string& n) {
        const Node* o = doc.find(inst);
        if (o && o->contains("children")) for (const auto& c : o->at("children").as_seq()) if (get_string(c, "name") == n) return SceneDocument::id_of(c);
        return ObjectId(0);
    };
    const ObjectId part = part_of(lamp, part_name);
    expect(part != 0 && doc.is_inherited(part), "the sphere the instance inherits has a node of its own");
    expect(app.sync().live(part) != nullptr, "...mapped to its live object");
    auto saved_instance = [&]() {
        app.save_scene();
        const Node file = coopa::yaml::load_document(doc.path());
        for (const auto& o : file.at("scene").at("root_objects").as_seq()) {
            if (get_string(o, "name") == name_of(lamp)) return Node(o);
        }
        return Node::mapping();
    };
    expect(!saved_instance().contains("children"), "an inherited part with no overrides is not written to the scene");

    // Picking: the part's mesh is hit; the first click selects the instance, the next the part.
    auto* live_part = app.sync().live(part);
    glm::vec2 px;
    const float s = std::max(1.0f, engine.display_scale());
    if (live_part && engine.world_to_window(glm::vec3(live_part->get_transform()->transform().get_world_matrix()[3]), px)) {
        const ObjectId hit = app.pick_object(px / s);
        expect(hit == part, "clicking the instance's sphere hits it (got " + name_of(hit) + ")");
        doc.clear_selection();
        expect(app.click_target(hit) == lamp, "the first click selects the whole instance");
        doc.select(lamp);
        expect(app.click_target(hit) == part, "a click on the selected instance selects the part");
    } else {
        expect(false, "the instance's sphere is on screen");
    }

    // A field override: only that key is saved, on the part, and the asset is untouched.
    Node mr = app.shown_component(part, "MeshRenderer");
    expect(mr.is_mapping() && mr.contains("mesh_path"), "the part shows the asset's MeshRenderer");
    mr["lod_bias"] = make_float(3.0f);
    ObjectId cube = 0;
    for (ObjectId id : doc.all_ids()) if (name_of(id) == "cube") cube = id;
    const auto* cube_live = app.sync().live(cube);
    app.edit_component(part, mr);
    tick(engine, 3);
    expect(app.has_overrides(part), "the part now has an override");
    expect(cube_live && app.sync().live(cube) == cube_live, "an override rebuilds only its instance (the cube's live object is untouched)");
    {
        auto* lp = app.sync().live(part);
        auto* lmr = lp ? lp->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
        expect(lmr && std::abs(lmr->lod_bias - 3.0f) < 1e-4f, "...and the rebuilt part has the overridden value live");
    }
    expect(std::abs(get_float(app.shown_component(part, "MeshRenderer"), "lod_bias") - 3.0f) < 1e-4f, "...and shows the overridden value");
    {
        const Node inst = saved_instance();
        bool ok = false;
        if (inst.contains("children") && inst.at("children").size() == 1) {
            const Node& c = inst.at("children")[0];
            ok = get_string(c, "name") == part_name && c.at("components").size() == 1 &&
                 component_type(c.at("components")[0]) == "MeshRenderer" && c.at("components")[0].size() == 2;   // type + lod_bias
        }
        expect(ok, "the scene saves just {type: MeshRenderer, lod_bias} under the part's name:\n" + coopa::yaml::emit(inst));
    }
    expect(coopa::yaml::emit(coopa::yaml::load_document(asset_file)) == asset_before, "the object asset itself is unchanged");
    doc.select(part);
    app.set_prop_tab(PropTab::Components);
    tick(engine, 3);
    dump(engine, "instance_part_override");   // the overridden field's marker (EDITOR_DUMP_DIR)

    // Moving the part: a Transform override holding only what changed.
    glm::vec3 p, r, sc;
    expect(app.object_transform(part, p, r, sc) && glm::distance(p, glm::vec3(0, 0, 1.5f)) < 1e-4f, "the part's transform shows the asset's pose");
    app.set_object_transform(part, p + glm::vec3(1, 0, 0), r, sc);
    tick(engine, 2);
    {
        const Node inst = saved_instance();
        Node t;
        for (const auto& c : inst.at("children")[0].at("components").as_seq()) if (component_type(c) == "Transform") t = c;
        expect(t.is_mapping() && t.contains("position") && !t.contains("rotation") && !t.contains("scale"),
               "moving the part overrides its position only");
    }
    expect(glm::distance(glm::vec3(app.sync().live(part)->get_transform()->transform().position()), glm::vec3(1, 0, 1.5f)) < 1e-4f,
           "...and moves the live part");

    // Revert one field.
    app.revert_override_field(part, "MeshRenderer", "lod_bias");
    tick(engine, 2);
    expect(std::abs(get_float(app.shown_component(part, "MeshRenderer"), "lod_bias", 1.0f) - 1.0f) < 1e-4f, "Revert brings back the asset's value");

    // Apply to Object Asset: the asset changes, this instance drops the override, others follow.
    mr = app.shown_component(part, "MeshRenderer");
    mr["lod_bias"] = make_float(2.0f);
    app.edit_component(part, mr);
    app.apply_component_to_asset(part, "MeshRenderer");
    tick(engine, 3);
    {
        const Node a = coopa::yaml::load_document(asset_file).at("object");
        float bias = 0.0f;
        for (const auto& c : a.at("children")[0].at("components").as_seq()) if (component_type(c) == "MeshRenderer") bias = get_float(c, "lod_bias", 1.0f);
        expect(std::abs(bias - 2.0f) < 1e-4f, "Apply to Object Asset writes the value into the asset");
    }
    const ObjectId second = app.place_object_asset(ref + ".yaml");
    tick(engine, 3);
    expect(std::abs(get_float(app.shown_component(part_of(second, part_name), "MeshRenderer"), "lod_bias", 1.0f) - 2.0f) < 1e-4f,
           "another instance shows the applied value");
    expect(std::abs(get_float(app.shown_component(part, "MeshRenderer"), "lod_bias", 1.0f) - 2.0f) < 1e-4f &&
           !app.shown_component(part, "MeshRenderer").is_null(), "this instance shows it from the asset");

    // Revert everything on the instance.
    app.revert_all_overrides(lamp);
    tick(engine, 2);
    expect(!app.has_overrides(part) && app.object_transform(part, p, r, sc) && glm::distance(p, glm::vec3(0, 0, 1.5f)) < 1e-4f,
           "Revert Instance Overrides restores the asset's part");
    expect(!saved_instance().contains("children"), "...and the scene holds no overrides for it");

    // Renaming the part in the asset carries the scene's override along.
    mr = app.shown_component(part, "MeshRenderer");
    mr["lod_bias"] = make_float(5.0f);
    app.edit_component(part, mr);
    tick(engine, 2);
    app.save_scene();
    const fs::path scene_file = doc.path();
    expect(app.open_object_asset(asset_file), "the object asset opens");
    tick(engine, 2);
    ObjectId asset_part = 0;
    for (ObjectId id : doc.all_ids()) if (name_of(id) == part_name) asset_part = id;
    expect(asset_part != 0, "the part is in the object asset");
    app.sync().apply(engine, doc, doc.set_object_key(asset_part, "name", Node(std::string("globe")), "Rename"));
    app.save_scene();
    expect(app.open_scene(scene_file), "the scene reopens");
    tick(engine, 3);
    const ObjectId globe = part_of(lamp, "globe");
    expect(globe != 0 && part_of(lamp, part_name) == 0, "the instance's part is now called globe");
    expect(std::abs(get_float(app.shown_component(globe, "MeshRenderer"), "lod_bias", 1.0f) - 5.0f) < 1e-4f,
           "...and still carries its override (the scene was rewritten)");
}

/**
 * @brief In an opened object asset, its meshes pick and enter Edit Mode like any scene object's:
 *        in the robot arm rig (with the Timeline open and posing it) a real click selects the
 *        clicked mesh and Tab edits the mesh it uses; the skinned tentacle's mesh picks, edits
 *        live and weight-paints.
 */
COOPA_TEST(an_object_assets_meshes_pick_and_enter_edit_mode) {
    using coopa::input::Key;
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4,
                     .prepare = [](Project& p) { copy_rig_assets(p.assets()); }});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& in = session.in;
    auto& project = session.project;
    const float scale = in.scale;
    expect(app.open_object_asset(project.assets() / "objects" / "robot_arm.yaml"), "the robot arm opens");
    app.show_timeline();
    tick(engine, 6);
    for (const char* name : {"base_plate", "upper_arm_shape"}) {
        const ObjectId target = object_named(app, name);
        const auto px = screen_of(engine, app, target, glm::vec3(0.0f, -0.5f, 0.0f), in.scale);   // the shape's front face
        expect(px.has_value(), std::string(name) + " is on screen");
        if (!px) continue;
        in.click(*px);
        tick(engine, 2);
        expect(app.document().primary() == target, std::string("clicking ") + name + " selects it (selected: " +
               (app.document().primary() ? get_string(*app.document().find(app.document().primary()), "name") : std::string("nothing")) + ")");
        in.move(*px);
        in.key(Key::Tab);
        tick(engine, 2);
        expect(app.interaction_mode() == InteractionMode::Edit && !app.mesh_document().mesh.faces.empty(),
               std::string("Tab enters Edit Mode on ") + name + "'s mesh");
        in.key(Key::Tab);
        tick(engine, 2);
        expect(app.interaction_mode() == InteractionMode::Object, "Tab returns to Object Mode");
    }
    dump(engine, "21_object_click");

    // The skinned tentacle: its mesh (a SkinnedMeshRenderer's) picks, edits and weight-paints.
    // At rest: the Timeline (still open from the arm) would otherwise pose the tentacle with its
    // clip, bending it so a mesh-space lift no longer reads as a rise of its top.
    app.set_timeline_rest_pose(true);
    expect(app.open_object_asset(project.assets() / "objects" / "tentacle.yaml"), "the tentacle opens");
    tick(engine, 6);
    const ObjectId skin = object_named(app, "tentacle_skin");
    const auto spx = screen_of(engine, app, skin, glm::vec3(0.0f, -0.2f, 0.6f), scale);   // the tube's front, low down
    expect(spx && app.pick_object(*spx) == skin, "clicking the skinned tentacle picks it");
    app.document().select(skin);
    tick(engine, 2);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Tab enters Edit Mode on the skinned mesh");
    tick(engine, 2);
    auto& md = app.mesh_document();
    expect(md.mesh.groups.size() == 4 && md.mesh.has_colors, "...with its vertex groups and colours");
    auto* smr = app.sync().live(skin)->get_component<toy::scene::SkinnedMeshRenderer>();
    float top_before = -1e9f;
    for (const auto& v : smr->skinned_vertices()) top_before = std::max(top_before, v.position.z);
    md.edit("Raise", [](EditMesh& m, MeshSelection&) { for (auto& p : m.positions) p.z += 0.5f; });
    tick(engine, 4);
    float top_after = -1e9f;
    for (const auto& v : smr->skinned_vertices()) top_after = std::max(top_after, v.position.z);
    expect(top_after > top_before + 0.4f, "an edit reaches the live skinned mesh (top " + std::to_string(top_before) + " -> " +
                                              std::to_string(top_after) + ")");
    expect(app.set_interaction_mode(InteractionMode::WeightPaint) && app.edited_mesh_shader() == "editor_paint",
           "Weight Paint shows the tentacle's groups");
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 2);
}

/**
 * @brief A terrain tile style is a tile-set object (assets/objects/tileset_*.yaml): toyengine's are
 *        listed among its Objects, open like any object, and duplicate into a NEW look that owns a
 *        copy of every piece mesh -- so reshaping a piece of the copy never touches the original.
 */
COOPA_TEST(a_duplicated_tile_set_owns_its_piece_meshes) {
    EditorSession session({.fixed_dt = "0"});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;

    const auto objects = app.engine_assets_listed(AssetType::Object);
    expect(std::find(objects.begin(), objects.end(), "objects/terrain/tileset_round.yaml") != objects.end(),
           "toyengine's tile sets are listed among its objects");

    const std::string copy = app.duplicate_tile_set("objects/terrain/tileset_round.yaml");
    expect(copy == "objects/tileset_round_copy.yaml", "the duplicate lands in the project under a new name");
    const Node doc = coopa::yaml::load_document(project.assets() / "objects" / "tileset_round_copy.yaml");
    const Node& obj = doc.at("object");
    expect(obj.at("name").get_value<std::string>() == "tileset_round_copy", "the copy names itself");
    int pieces = 0, own = 0;
    for (const auto& child : obj.at("children")) {
        for (const auto& comp : child.at("components")) {
            if (!comp.contains("mesh_path")) continue;
            ++pieces;
            const std::string ref = comp.at("mesh_path").get_value<std::string>();
            if (ref == "tileset_round_copy_" + child.at("name").get_value<std::string>() &&
                fs::exists(project.assets() / "meshes" / (ref + ".yaml"))) ++own;
        }
    }
    expect(pieces == static_cast<int>(toy::world::k_tile_piece_count) && own == pieces, "every piece of the copy points at its own new mesh file");
    expect(fs::exists(Project::engine_assets() / "meshes" / "terrain" / "round" / "tile_round_top_outer.yaml"),
           "toyengine's own pieces are left in place");

    app.open_asset(AssetType::Object, copy);
    tick(engine, 4);
    expect(app.active_asset_type() == AssetType::Object, "the new tile set opens as an object, pieces and all");
}

} // namespace toy::editor::testing
