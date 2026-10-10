/**
 * @file mesh_editing_test.cpp
 * @brief Editing a scene object's mesh edits the mesh ASSET: every user follows, leaving Edit Mode
 * saves it, Object Mode's Ctrl+Z walks one timeline across the scene and every mesh edited from it,
 * saving refreshes colliders built from the file, water bodies' meshes are editable, and Shade
 * Flat / Smooth reach the file, the render and undo.
 */

#include <coopa/testing/test.h>

#include <gfxcoopa/util/image_readback.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("mesh_editing");

namespace toy::editor::testing {

namespace {

/** @brief Faces of a mesh file on disk. */
size_t faces_on_disk(const fs::path& p) { return mesh_from_node(coopa::yaml::load_document(p)).faces.size(); }

/** @brief Index count of the live renderer of the document object named `name`. */
uint32_t live_indices(EditorApp& app, const std::string& name) {
    for (const auto& [id, live] : app.sync().live_objects()) {
        if (!live || live->name() != name) continue;
        auto* mr = live->get_component<coopa::gfx::engine::components::MeshRenderer>();
        return mr && mr->is_ready() ? mr->get_mesh()->index_count() : 0u;
    }
    return 0u;
}

} // namespace

/**
 * @brief Editing a scene object's mesh edits the mesh ASSET, not a copy: every object using it
 *        follows. It is saved automatically on leaving Edit Mode, and the edit stays undoable
 *        afterwards -- Object Mode's Ctrl+Z walks one timeline across the
 *        scene and every mesh edited from it (across meshes, in order), keeping files and the
 *        live scene in step; redo walks it back.
 */
COOPA_TEST(mesh_edits_autosave_and_share_one_undo_timeline) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;
    auto& doc = app.document();
    const fs::path cube_file = project.assets() / "meshes" / "cube.yaml";
    const ObjectId cube = object_named(app, "cube");

    // 1. Edit the cube (a duplicate shares its mesh), leave Edit Mode: saved without asking.
    doc.select(cube);
    app.duplicate_selected();
    tick(engine, 3);
    doc.select(cube);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode on the cube");
    expect(app.mesh_document().path.filename() == "cube.yaml", "it loads the object's mesh asset (meshes/cube.yaml)");
    app.mesh_document().selection.mode = SelectMode::Face;
    app.mesh_document().selection.faces = {1};
    app.mesh_document().edit("Extrude", [](EditMesh& m, MeshSelection& sel) { extrude_faces(m, sel, 0.5f); });
    tick(engine, 3);
    size_t updated = 0;
    for (const auto& [id, live] : app.sync().live_objects()) {
        auto* mr = live ? live->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
        if (mr && mr->is_ready() && mr->get_mesh()->index_count() == 10u * 6u) ++updated;
    }
    expect(updated == 2, "both objects using meshes/cube show the edit (" + std::to_string(updated) + ")");
    app.set_interaction_mode(InteractionMode::Object);
    expect(faces_on_disk(cube_file) == 10u, "leaving Edit Mode saved the cube's mesh");
    expect(!app.mesh_document().dirty(), "...and nothing is left unsaved");

    // 2. A second mesh (a plane), edited and left the same way.
    const ObjectId plane = app.create_primitive("Plane");
    tick(engine, 3);
    doc.select(plane);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode on the plane");
    const fs::path plane_file = app.mesh_document().path;
    const size_t plane_faces = app.mesh_document().mesh.faces.size();
    app.mesh_document().selection.mode = SelectMode::Face;
    app.mesh_document().selection.faces = {0};
    app.mesh_document().edit("Extrude", [](EditMesh& m, MeshSelection& sel) { extrude_faces(m, sel, 0.3f); });
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 2);
    expect(faces_on_disk(plane_file) == plane_faces + 4u, "leaving Edit Mode saved the plane's mesh");

    // 3. Then a scene edit: move the cube.
    const int ti = doc.find_component(cube, "Transform");
    Node t = doc.find(cube)->at("components").as_seq()[static_cast<size_t>(ti)];
    t["position"] = make_vec3(glm::vec3(3.0f, 0.0f, 0.0f));
    doc.set_component(cube, ti, t, "Move");
    tick(engine, 2);

    // Object Mode undo, newest first: the move, the plane's extrude, the plane's creation (a scene
    // step), then the cube's extrude.
    app.undo();
    tick(engine, 3);
    expect(glm::vec3(get_vec3(doc.find(cube)->at("components").as_seq()[static_cast<size_t>(ti)], "position")).x == 0.0f,
           "undo 1: the move");
    expect(faces_on_disk(plane_file) == plane_faces + 4u && faces_on_disk(cube_file) == 10u, "...meshes untouched");
    app.undo();
    tick(engine, 3);
    expect(faces_on_disk(plane_file) == plane_faces, "undo 2: the plane's extrude (file reverted)");
    expect(faces_on_disk(cube_file) == 10u, "...the cube's still there");
    app.undo();
    tick(engine, 3);
    expect(doc.find(plane) == nullptr, "undo 3: the plane's creation");
    app.undo();
    tick(engine, 3);
    expect(faces_on_disk(cube_file) == 6u, "undo 4: the cube's extrude, across the mesh switch (history kept)");
    expect(live_indices(app, "cube") == 6u * 6u, "...and the live cube shows it");

    // Redo walks it back in order.
    app.redo();
    tick(engine, 3);
    expect(faces_on_disk(cube_file) == 10u && live_indices(app, "cube") == 10u * 6u, "redo 1: the cube's extrude");
    app.redo();
    tick(engine, 3);
    expect(doc.find(plane) != nullptr, "redo 2: the plane's creation");
    app.redo();
    tick(engine, 3);
    expect(faces_on_disk(plane_file) == plane_faces + 4u, "redo 3: the plane's extrude");
    app.redo();
    tick(engine, 3);
    expect(glm::vec3(get_vec3(doc.find(cube)->at("components").as_seq()[static_cast<size_t>(ti)], "position")).x == 3.0f,
           "redo 4: the move");

    // Re-entering a mesh resumes its history: Edit Mode's own Ctrl+Z still reaches the extrude.
    doc.select(cube);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode on the cube again");
    expect(app.mesh_document().undo.can_undo(), "...with its earlier history");
    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief Saving a mesh refreshes every asset built from the file at once -- a MeshCollider too. */
COOPA_TEST(saving_a_mesh_refreshes_its_colliders) {
    EditorSession session({.prepare = [](Project& project) {
        coopa::yaml::save_document(project.assets() / "meshes" / "floor_col.yaml", mesh_to_node(make_plane(4.0f, 1)));
    }});
    auto& engine = session.engine;
    auto& app = session.app;
    const fs::path col_file = session.project.assets() / "meshes" / "floor_col.yaml";
    auto& doc = app.document();
    const ObjectId floor = app.create_with_component("MeshCollider", "Floor");
    const int ci = doc.find_component(floor, "MeshCollider");
    Node comp = doc.find(floor)->at("components").as_seq()[static_cast<size_t>(ci)];
    comp["mesh_path"] = Node(std::string("floor_col"));
    doc.set_component(floor, ci, comp, "Collider mesh");
    app.sync().rebuild(engine, doc);
    tick(engine, 4);
    auto triangles = [&]() -> size_t {
        for (const auto& [id, live] : app.sync().live_objects()) {
            if (id != floor || !live) continue;
            auto* mc = live->get_component<coopa::physx::components::MeshCollider>();
            return mc && mc->mesh().is_loaded() ? mc->mesh()->triangle_count() : 0u;
        }
        return 0u;
    };
    expect(triangles() == 2u, "the collider loaded its 1-quad mesh");
    expect(app.open_mesh(col_file), "open the collider's mesh as an asset");
    app.mesh_document().edit("Subdivide", [](EditMesh& m, MeshSelection&) { m = make_plane(4.0f, 3); });
    expect(app.save_mesh(), "save it");
    expect(triangles() == 18u, "the live collider has the saved mesh immediately (" + std::to_string(triangles()) + " tris)");
}

/** @brief Water bodies are editable meshes too: Edit Mode on a WaterBody object edits its
 *         WaterBody's mesh (a procedural grid is written out to a mesh file first), the live
 *         water re-bakes from the unsaved edit, and saving writes the file. */
COOPA_TEST(a_water_body_mesh_is_editable) {
    EditorSession session;
    auto& engine = session.engine;
    auto& app = session.app;

    // A plane turned into a grid-based water body: MeshRenderer keeps only the material.
    const ObjectId pond = app.create_primitive("Plane");
    auto& doc = app.document();
    const int mr_ci = doc.find_component(pond, "MeshRenderer");
    Node mr = doc.find(pond)->at("components").as_seq()[static_cast<size_t>(mr_ci)];
    mr["mesh_path"] = Node(std::string(""));
    doc.set_component(pond, mr_ci, mr, "No mesh");
    Node water = Node::mapping();
    water["type"] = Node(std::string("WaterBody"));
    water["mode"] = Node(std::string("planar"));
    Node size = Node::mapping();
    size["x"] = make_float(6.0);
    size["y"] = make_float(4.0);
    water["size"] = size;
    water["resolution"] = Node(static_cast<int64_t>(6));
    doc.add_component(pond, water);
    app.sync().rebuild(engine, doc);
    tick(engine, 3);

    doc.select(pond);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode on a WaterBody object");
    auto& md = app.mesh_document();
    const int w_ci = doc.find_component(pond, "WaterBody");
    const std::string key = get_string(doc.find(pond)->at("components").as_seq()[static_cast<size_t>(w_ci)], "mesh_path");
    expect(!key.empty() && md.path.filename() == key + ".yaml",
           "the grid was written to a mesh file and the WaterBody points at it (" + key + ")");
    expect(md.mesh.positions.size() == 7u * 7u && md.mesh.faces.size() == 36u, "...the same 6x6 grid the water drew");

    // Raise the whole surface by a metre: the live water re-bakes from the unsaved edit.
    md.selection.mode = SelectMode::Vertex;
    md.edit("Raise", [](EditMesh& m, MeshSelection&) { for (auto& p : m.positions) p.z += 1.0f; });
    tick(engine, 4);
    toy::water::WaterBody* live = nullptr;
    for (const auto& [id, obj] : app.sync().live_objects()) {
        if (id == pond && obj) live = obj->get_component<toy::water::WaterBody>();
    }
    const glm::vec3 base = live && live->owner
        ? glm::vec3(live->owner->get_transform()->transform().get_world_matrix()[3]) : glm::vec3(0.0f);
    expect(live && live->baked && std::fabs(live->query.bounds_max().z - (base.z + 1.0f)) < 1e-3f,
           "the live water shows the edit before saving");

    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 4);
    live = nullptr;
    for (const auto& [id, obj] : app.sync().live_objects()) {
        if (id == pond && obj) live = obj->get_component<toy::water::WaterBody>();
    }
    expect(live && live->baked && std::fabs(live->query.bounds_max().z - (base.z + 1.0f)) < 1e-3f,
           "leaving Edit Mode keeps showing the unsaved edit (z max " +
               std::to_string(live ? live->query.bounds_max().z : -1.0f) + ")");
    glm::vec3 olo(0.0f), ohi(0.0f);
    expect(app.outline_bounds(pond, olo, ohi) && std::fabs(ohi.z - 1.0f) < 1e-4f,
           "...and so does its selection outline (drawn from the open mesh, not the file on disk)");
    expect(app.save_mesh(), "saving writes the water's mesh");
    const EditMesh saved = mesh_from_node(coopa::yaml::load_document(md.path));
    expect(!saved.positions.empty() && std::fabs(saved.positions[0].z - 1.0f) < 1e-5f, "...with the edit in it");

    // Re-entering edits the same file -- no second conversion.
    doc.select(pond);
    expect(app.set_interaction_mode(InteractionMode::Edit), "Edit Mode again");
    expect(app.mesh_document().path.filename() == key + ".yaml", "...on the same mesh file");
    app.set_interaction_mode(InteractionMode::Object);
}

/** @brief Object Mode's Shade Flat / Shade Smooth: the mesh file, the render and undo agree. */
COOPA_TEST(shade_flat_and_smooth_reach_the_file_render_and_undo) {
    EditorSession session({.settle_frames = toy::core::Engine::kFillDebounceFrames + 4});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& project = session.project;
    const glm::vec3 centre(0, 0, 2);
    const ObjectId sphere = app.create_primitive("Sphere");
    app.document().set_transform(sphere, centre, {0, 0, 0}, glm::vec3(1.5f), "Move");
    app.sync().apply(engine, app.document(), {ChangeScope::Transform, sphere});
    tick(engine, 6);
    const fs::path mesh_file = project.assets() / "meshes/sphere.yaml";
    auto smooth_faces = [&]() {
        const EditMesh m = mesh_from_node(coopa::yaml::load_document(mesh_file));
        size_t n = 0;
        for (const auto& f : m.faces) n += f.smooth ? 1 : 0;
        return std::pair<size_t, size_t>{n, m.faces.size()};
    };
    // Facet edges: shading steps between neighbouring pixels inside the sphere's disc (the
    // middle of it, clear of the silhouette). A whole-image diff would not do: exposure and
    // the shadow fit drift between captures of the same scene by thousands of pixels.
    auto facet_edges = [&]() {
        app.document().clear_selection();   // the selection wireframe would cover the shading
        tick(engine, 30);
        glm::vec2 c, top;
        if (!engine.world_to_window(centre, c) || !engine.world_to_window(centre + glm::vec3(0, 0, 0.75f), top)) return size_t(0);
        const auto d = engine.display_rect();
        const coopa::gfx::util::ImageData img = engine.capture_image(true);
        const float sx = float(img.width) / float(d.w), sy = float(img.height) / float(d.h);
        const int cx = int((c.x - d.x) * sx), cy = int((c.y - d.y) * sy);
        const int r = int(glm::distance(c, top) * sy * 0.6f);
        auto lum = [&](int x, int y) {
            const size_t i = (size_t(y) * img.width + size_t(x)) * img.channels;
            return int(img.pixels[i]) + int(img.pixels[i + 1]) + int(img.pixels[i + 2]);
        };
        size_t n = 0;
        for (int y = std::max(0, cy - r); y < std::min(int(img.height) - 1, cy + r); ++y)
            for (int x = std::max(0, cx - r); x < std::min(int(img.width) - 1, cx + r); ++x)
                if (std::abs(lum(x + 1, y) - lum(x, y)) >= 24 || std::abs(lum(x, y + 1) - lum(x, y)) >= 24) ++n;
        return n;
    };
    const auto [s0, total] = smooth_faces();
    expect(total > 0 && s0 == total, "a new UV sphere is smooth-shaded");
    const size_t smooth_edges = facet_edges();

    app.document().select(sphere);
    app.shade_selected(false);
    expect(smooth_faces().first == 0, "Shade Flat marks every face of the mesh flat, in its file");
    const size_t flat_edges = facet_edges();
    expect(flat_edges > smooth_edges * 3 / 2 + 50, "the render shows the facets (" + std::to_string(smooth_edges) +
                                                   " -> " + std::to_string(flat_edges) + " facet edges)");

    app.undo();
    expect(smooth_faces().first == total, "Ctrl+Z in Object Mode makes it smooth again");
    const size_t undone_edges = facet_edges();
    expect(undone_edges < (smooth_edges + flat_edges) / 2, "and the render is smooth again (" +
                                                         std::to_string(undone_edges) + " facet edges)");

    app.document().select(sphere);
    app.shade_selected(false);
    app.shade_selected(true);
    expect(smooth_faces().first == total, "Shade Smooth marks every face smooth");
    expect(facet_edges() < (smooth_edges + flat_edges) / 2, "and renders smooth");
}

} // namespace toy::editor::testing
