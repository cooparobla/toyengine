/**
 * @file editor_end_to_end_test.cpp
 * @brief The editor end to end on a new project: create, transform, pick, assign a material asset,
 * shade, edit a mesh, apply render settings live, play / stop, save, rebuild the renderer and
 * restart the engine with unsaved edits -- then the plain game Engine loads what was saved. And
 * files the editor creates (meshes, a material, a rig with a keyed clip) run through Play.
 */

#include <coopa/testing/test.h>

#include <gfxcoopa/util/image_readback.h>

#include "editor/support/editor_session.h"
#include <toyengine/render/toy_render_pipeline.h>

COOPA_TEST_SUITE("editor_end_to_end");

namespace toy::editor::testing {

/**
 * @brief One session the way a user works -- create, transform, pick, assign a material asset,
 *        switch shading, edit a mesh asset, apply a render setting, play / stop, save -- then
 *        Rebuild Renderer and a full engine restart keep the unsaved edits (and their undo), and
 *        the plain game Engine loads the saved project.
 */
COOPA_TEST(create_edit_save_restart_and_load_in_the_game) {
    setenv("FIXED_DT", "0", 1);
    setenv("NO_INPUT", "1", 1);
    Project project = new_project();
    expect(project.valid() && coopa::yaml::document_exists(project.assets() / "scenes/main/scene.yaml"), "a new project has a scene");

    fs::path scene_path;
    toy::core::AppConfig restart_config;
    EditorState restart_state;
    ObjectId restart_sphere{};   // the sphere's id, carried across the restart with the document
    {
        toy::core::Engine engine(shell_config(project), shell_options(project));
        EditorApp app(engine, project);
        tick(engine, 4);
        expect(app.document().all_ids().size() == 4, "the default scene opened (camera, sun, ground, cube)");
        expect(app.sync().scene() && !app.sync().scene()->is_simulating(), "the live scene is in edit mode");
        dump(engine, "01_scene_solid");

        // Create, select, transform, save.
        const ObjectId sphere = app.create_primitive("Sphere");
        tick(engine, 2);
        expect(app.document().find(sphere) && app.sync().live(sphere), "a created primitive exists in the document and live scene");
        expect(coopa::yaml::document_exists(project.assets() / "meshes/sphere.yaml"), "its mesh asset exists");
        app.document().set_transform(sphere, {2, 1, 0.5f}, {0, 0, 45}, glm::vec3(1), "Move");
        app.sync().apply(engine, app.document(), {ChangeScope::Transform, sphere});
        tick(engine, 2);
        expect(glm::distance(app.sync().live(sphere)->get_transform()->transform().position(), glm::vec3(2, 1, 0.5f)) < 1e-5f,
               "a transform edit reaches the live object without a rebuild");

        // Picking: the sphere's projected centre picks the sphere. (From the default view the
        // starter scene's camera sits right in front of it, so look from the other side.)
        app.camera().yaw_deg = -50.0f;
        app.camera().apply();
        tick(engine, 2);
        auto* cam = coopa::gfx::engine::components::CameraComponent::main();
        expect(cam != nullptr, "a main camera (the editor camera) exists");
        glm::vec2 px;
        if (engine.world_to_window(glm::vec3(2, 1, 0.5f), px)) {
            const float s = std::max(1.0f, engine.display_scale());
            const ObjectId got = app.pick_object(px / s);
            expect(got == sphere, "clicking the sphere's centre picks it (got " +
                   (got && app.document().find(got) ? get_string(*app.document().find(got), "name") : std::string("nothing")) + ")");
        }

        // Material asset assigned by reference.
        expect(app.create_material("brick"), "a material asset can be created");
        app.material_document().node["albedo"] = make_color({0.7f, 0.2f, 0.1f});
        app.save_material();
        app.show_document_view();
        tick(engine, 2);
        const int ci = app.document().find_component(sphere, "MeshRenderer");
        Node comp = app.document().find(sphere)->at("components").as_seq()[ci];
        comp["material"] = Node(std::string("materials/brick"));
        app.document().set_component(sphere, ci, comp, "Assign");
        app.sync().apply(engine, app.document(), {ChangeScope::Object, sphere});
        tick(engine, 2);
        auto* mr = app.sync().live(sphere)->get_component<coopa::gfx::engine::components::MeshRenderer>();
        expect(mr && std::abs(mr->material.albedo.r - 0.7f) < 1e-4f, "a `material: materials/brick` reference loads the asset's values");

        // Shading modes render differently.
        app.set_shading(Shading::Solid);
        tick(engine, 3);
        const auto solid = engine.capture_image(true);
        app.set_shading(Shading::Wireframe);
        tick(engine, 3);
        const auto wire = engine.capture_image(true);
        app.set_shading(Shading::Full);
        tick(engine, 3);
        const auto full = engine.capture_image(true);
        dump(engine, "02_scene_full");
        auto differ = [](const coopa::gfx::util::ImageData& a, const coopa::gfx::util::ImageData& b) {
            if (a.pixels.size() != b.pixels.size()) return true;
            size_t n = 0;
            for (size_t i = 0; i < a.pixels.size(); i += 4) n += a.pixels[i] != b.pixels[i] || a.pixels[i + 1] != b.pixels[i + 1];
            return n > 100;
        };
        expect(differ(solid, wire) && differ(solid, full) && differ(wire, full), "wireframe, solid and full render differ");

        // Mesh editing in the Asset tab.
        app.new_mesh("Cube");
        tick(engine, 3);
        expect(app.active_asset_type() == AssetType::Mesh && app.mesh_document().mesh.faces.size() == 6, "a new cube mesh opens in the mesh viewer");
        {
            auto* po = engine.scene().find_object("PreviewObject");
            auto* pmr = po ? po->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
            expect(pmr && pmr->is_ready(), "the asset preview scene is active and shows the mesh");
        }
        auto& md = app.mesh_document();
        md.selection.mode = SelectMode::Face;
        md.selection.faces = {1};
        md.edit("Extrude", [](EditMesh& m, MeshSelection& s) { extrude_faces(m, s, 0.75f); });
        tick(engine, 3);
        expect(md.mesh.faces.size() == 10, "extrude through the document");
        dump(engine, "03_asset_mesh");
        app.set_shading(Shading::Solid);
        tick(engine, 3);
        dump(engine, "03b_asset_mesh_solid");
        app.set_shading(Shading::Full);
        md.do_undo();
        expect(md.mesh.faces.size() == 6, "mesh undo");
        md.do_redo();
        expect(app.save_mesh() && coopa::yaml::document_exists(project.assets() / "meshes" / (md.name + ".yaml")), "the mesh saves");

        // Render settings apply live.
        app.set_prop_tab(PropTab::Render);
        const Node before = app.config_document().node;
        app.config_document().section("render")["exposure"] = make_float(2.0);
        app.config_document().commit("exposure", before, {});
        app.apply_config_live();
        tick(engine, 2);
        expect(std::abs(engine.render_config().exposure - 2.0f) < 1e-5f, "a render setting edit applies to the live renderer");
        dump(engine, "04_render_settings");
        app.set_prop_tab(PropTab::Output);
        tick(engine, 2);
        dump(engine, "05_output_props");
        app.set_prop_tab(PropTab::World);
        tick(engine, 2);
        dump(engine, "05b_world_props");

        // Play / stop leaves the document alone.
        app.show_document_view();
        tick(engine, 2);
        const Node doc_before = app.document().node();
        app.play();
        tick(engine, 3);
        expect(app.playing() && engine.scene().is_simulating(), "play runs a simulating copy");
        app.stop();
        tick(engine, 2);
        expect(!app.playing() && app.document().node() == doc_before && !engine.scene().is_simulating(), "stop returns to the untouched edit scene");

        // Save.
        expect(app.save_scene(), "the scene saves");
        scene_path = app.document().path();

        // Rebuild Renderer: a startup-only setting takes effect in place -- same Engine and
        // window, documents untouched.
        const Node cb = app.config_document().node;
        app.config_document().section("render")["render_width"] = Node(int64_t(320));
        app.config_document().section("render")["render_height"] = Node(int64_t(180));
        app.config_document().commit("res", cb, {});
        app.document().set_object_key(sphere, "name", Node(std::string("Renamed")), "Rename");
        const int rebuilds = engine.pipeline_rebuild_count();
        app.rebuild_renderer();
        tick(engine, 3);
        expect(engine.pipeline().render_height() == 180 && engine.pipeline_rebuild_count() > rebuilds,
               "Rebuild Renderer applies startup-only render settings in place");
        expect(app.document().dirty() && app.document().find(sphere) && get_string(*app.document().find(sphere), "name") == "Renamed",
               "...with the open document untouched");

        // Restart Editor Engine (the full restart, for settings baked in at engine start-up).
        restart_config = app.config_for_restart();
        restart_state = app.take_state();
        restart_sphere = sphere;
    }
    {
        toy::core::AppConfig cfg = restart_config;
        cfg.window.visible = false;
        cfg.render.screen_ui_enabled = true;
        apply_editor_render_overrides(cfg);
        toy::core::Engine engine(cfg, shell_options(project));
        EditorApp app(engine, project, {}, std::move(restart_state));
        tick(engine, 3);
        expect(engine.pipeline().render_height() == 180, "a full engine restart keeps the edited render settings");
        expect(app.document().dirty() && app.document().find_component(restart_sphere, "MeshRenderer") >= 0,
               "unsaved scene edits survive the restart");
        app.show_document_view();
        tick(engine, 1);
        app.undo();
        tick(engine, 2);
        expect(engine.scene().find_object("sphere") != nullptr, "and remain undoable afterwards");
    }

    // The saved project loads in the plain game Engine.
    {
        toy::core::AppConfig cfg = shell_config(project);
        cfg.scene.default_scene = scene_path.string();
        toy::core::EngineOptions o;
        o.project_root = project.root();
        toy::core::Engine game(cfg, o);
        tick(game, 2);
        auto* obj = game.scene().find_object("sphere");
        expect(obj != nullptr, "the game finds the editor-created object");
        auto* mr = obj ? obj->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
        expect(mr && mr->get_mesh().is_loaded() && std::abs(mr->material.albedo.r - 0.7f) < 1e-4f,
               "with its mesh loaded and its material asset applied");
    }
}

/**
 * @brief Files the editor CREATES are legitimate: a scene built in the editor from primitive
 *        meshes, a new material and a rig with a keyed clip is saved, then run through Play --
 *        the engine's own loaders -- and every mesh builds, the material's values arrive, and
 *        the Animator plays its clip file.
 */
COOPA_TEST(editor_created_files_load_through_the_engine) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    std::vector<ObjectId> prims;
    for (const char* p : {"Cube", "Sphere", "Cylinder", "Plane", "Cone", "Torus", "Icosphere"}) {
        const ObjectId id = app.create_primitive(p);
        if (id) prims.push_back(id);
    }
    expect(prims.size() >= 4, "created primitive objects (" + std::to_string(prims.size()) + ")");
    Node mat = Node::mapping();
    mat["albedo"] = make_color(glm::vec3(0.1f, 0.6f, 0.3f));
    mat["metallic"] = make_float(0.25);
    mat["roughness"] = make_float(0.7);
    expect(app.create_material("painted", mat), "created a material asset");
    app.show_document_view();   // creating a material opens it; back to the scene
    tick(engine, 2);
    const int mr = doc.find_component(prims[0], "MeshRenderer");
    Node mrn = doc.find(prims[0])->at("components").as_seq()[static_cast<size_t>(mr)];
    mrn["material"] = Node(std::string("materials/painted"));
    doc.set_component(prims[0], mr, mrn, "Material");
    const ObjectId bone = doc.add_object(doc.make_object("Bone"), prims[0]);
    app.sync().rebuild(engine, doc);
    tick(engine, 2);
    doc.select(prims[0]);
    app.show_timeline();
    app.add_animator(prims[0]);
    app.new_animation_clip("Spin");
    tick(engine, 2);
    doc.select(bone);
    app.set_animation_record(true);
    tick(engine, 1);
    app.set_animation_time(0.0f);
    tick(engine, 1);
    app.insert_keyframes();
    app.set_animation_time(1.0f);
    tick(engine, 1);
    app.set_object_transform(bone, glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(1));
    app.insert_keyframes();
    app.set_animation_record(false);
    tick(engine, 1);
    expect(app.save_scene(), "the scene saves");

    // Play loads the saved document through the engine's own loaders.
    app.play();
    tick(engine, 30);
    expect(app.playing(), "Play started from the editor-built scene");
    auto& scene = engine.scene();
    int ready = 0, renderers = 0;
    for (auto* r : scene.get_components<coopa::gfx::engine::components::MeshRenderer>()) {
        if (!r->owner) continue;
        bool prim = false;
        for (const auto& [id, live] : app.sync().live_objects()) (void)id, (void)live;
        for (ObjectId pid : prims) prim |= r->owner->name() == get_string(*doc.find(pid), "name");
        if (!prim) continue;
        ++renderers;
        ready += r->is_ready() ? 1 : 0;
    }
    expect(renderers == static_cast<int>(prims.size()) && ready == renderers,
           "every editor-made mesh loads in the engine (" + std::to_string(ready) + "/" + std::to_string(renderers) + ")");
    auto* painted = scene.find_object(get_string(*doc.find(prims[0]), "name"));
    auto* pmr = painted ? painted->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(pmr && glm::distance(pmr->material.albedo, glm::vec3(0.1f, 0.6f, 0.3f)) < 1e-3f && std::abs(pmr->material.metallic - 0.25f) < 1e-4f &&
               std::abs(pmr->material.roughness - 0.7f) < 1e-4f,
           "the editor-made material's values reach the engine");
    auto* animator = painted ? painted->get_component<coopa::anim::Animator>() : nullptr;
    auto* live_bone = painted ? painted->find_descendant("Bone") : nullptr;
    expect(animator && animator->is_playing() && animator->current_state() == "Spin",
           "the editor-made Animator plays its clip state");
    expect(live_bone && live_bone->get_transform()->transform().position().z > 0.5f,
           "...and the clip file drives the bone (z = " +
               std::to_string(live_bone ? live_bone->get_transform()->transform().position().z : -1.0f) + ")");
    app.stop();
    tick(engine, 2);
}

} // namespace toy::editor::testing
