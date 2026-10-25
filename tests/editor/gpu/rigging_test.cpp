/**
 * @file rigging_test.cpp
 * @brief Binding a mesh to a rig end to end: Object > Parent > Bind to Rig with Automatic
 * Weights writes bone-named vertex groups into the mesh FILE, makes the object a skinned one,
 * and the live mesh then bends when a joint is rotated -- with no Weight Paint stroke and no
 * exported bone palette anywhere.
 */

#include <coopa/testing/test.h>

#include <toyengine/scene/skinned_mesh_renderer.h>

#include "editor/support/editor_session.h"

COOPA_TEST_SUITE("rigging");

namespace toy::editor::testing {

using coopa::test::require;

namespace {

/** @brief An open tube along +Z from 0 to `length`, enough rings to show a bend. */
EditMesh tube_mesh(float radius, float length, int sides, int rings) {
    EditMesh m;
    for (int r = 0; r < rings; ++r) {
        const float z = length * static_cast<float>(r) / static_cast<float>(rings - 1);
        for (int i = 0; i < sides; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(sides);
            m.positions.push_back({radius * std::cos(a), radius * std::sin(a), z});
        }
    }
    for (int r = 0; r + 1 < rings; ++r) {
        for (int i = 0; i < sides; ++i) {
            const uint32_t a = static_cast<uint32_t>(r * sides + i);
            const uint32_t b = static_cast<uint32_t>(r * sides + (i + 1) % sides);
            Face f;
            for (uint32_t v : {a, b, b + static_cast<uint32_t>(sides), a + static_cast<uint32_t>(sides)}) {
                f.corners.push_back(Corner{v, glm::vec2(0.0f), glm::vec4(1.0f), glm::vec3(0.0f), glm::vec4(0.0f), glm::vec3(0.0f)});
            }
            m.faces.push_back(std::move(f));
        }
    }
    m.sync_vertex_data();
    return m;
}

/** @brief The component node of `type` on `id`, or an empty mapping. */
Node component_of(SceneDocument& doc, ObjectId id, const std::string& type) {
    const int ci = doc.find_component(id, type);
    if (ci < 0) return Node::mapping();
    return doc.find(id)->at("components").as_seq()[static_cast<size_t>(ci)];
}

/** @brief The live SkinnedMeshRenderer on `id`, or null. */
toy::scene::SkinnedMeshRenderer* skinned(EditorApp& app, ObjectId id) {
    auto* live = app.sync().live(id);
    return live ? live->get_component<toy::scene::SkinnedMeshRenderer>() : nullptr;
}

} // namespace

/**
 * @brief The whole bind: a plain mesh object under a bone chain becomes a skinned one whose
 *        weights came out of the generator, and rotating a joint bends it.
 */
COOPA_TEST(bind_to_rig_with_automatic_weights_deforms_the_mesh) {
    EditorSession session({.prepare = [](Project& project) {
        coopa::yaml::save_document(project.assets() / "meshes" / "limb.yaml",
                                   mesh_to_node(tube_mesh(0.3f, 3.0f, 12, 13)));
    }});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    // Rig > Bone0 (at the base) > Bone1 (half way up) > Bone2 (the tip), and the skin beside them.
    const ObjectId rig = doc.add_object(doc.make_object("Rig"));
    const ObjectId bone0 = doc.add_object(doc.make_object("Bone0"), rig);
    const ObjectId bone1 = doc.add_object(doc.make_object("Bone1"), bone0);
    const ObjectId bone2 = doc.add_object(doc.make_object("Bone2"), bone1);
    doc.set_transform(bone0, {0, 0, 0}, {}, {1, 1, 1}, "rest");
    doc.set_transform(bone1, {0, 0, 1.5f}, {}, {1, 1, 1}, "rest");
    doc.set_transform(bone2, {0, 0, 1.5f}, {}, {1, 1, 1}, "rest");
    const ObjectId skin = doc.add_object(doc.make_object("Skin"), rig);
    Node renderer = default_component("MeshRenderer");
    renderer["mesh_path"] = Node(std::string("limb"));
    doc.add_component(skin, renderer);
    app.sync().rebuild(engine, doc);
    tick(engine, 2);
    require(skinned(app, skin) == nullptr, "a plain mesh object is not skinned to start with");

    doc.select(skin);
    require(app.bind_selection_to_rig(BindWeights::Automatic), "the selection binds to the rig above it");
    app.sync().rebuild(engine, doc);
    tick(engine, 8);   // the new SkinnedMeshRenderer loads its bind pose asynchronously

    // The object became a skinned one, the engine's way round: the mesh on the
    // SkinnedMeshRenderer, the material alone on the MeshRenderer.
    const Node smr = component_of(doc, skin, "SkinnedMeshRenderer");
    expect(get_string(smr, "mesh_path") == "limb", "bind: the mesh moves onto a SkinnedMeshRenderer");
    expect(get_string(component_of(doc, skin, "MeshRenderer"), "mesh_path").empty(),
           "bind: ...and off the MeshRenderer, which now supplies the material alone");
    expect(get_string(smr, "rig").empty(), "bind: no `rig:` is written when the engine finds this one by itself");

    // The weights are in the mesh FILE, as vertex groups named after the bones.
    const fkyaml::node file = coopa::yaml::load_document(session.project.assets() / "meshes" / "limb.yaml");
    require(file.contains("weights"), "bind: the mesh file gains a weights block");
    std::set<std::string> named;
    for (const auto& per_vertex : file.at("weights")) {
        if (!per_vertex.is_mapping()) continue;
        for (auto item : per_vertex.map_items()) named.insert(item.key().get_value<std::string>());
    }
    expect(named.count("Bone0") && named.count("Bone1") && named.count("Bone2") && !named.count("Rig") && !named.count("Skin"),
           "bind: the groups are named after the rig's joints, and nothing else");
    expect(!file.contains("joints") && !file.contains("inverse_bind_matrices"),
           "bind: and carry no exported bone palette to outrank them");

    // The live mesh is skinned against the rig, and bends when a joint turns.
    auto* live_skin = skinned(app, skin);
    require(live_skin != nullptr && live_skin->is_ready(), "bind: the live object skins its mesh");
    expect(live_skin->bones().size() >= 3, "bind: every group resolved to a bone of the rig");
    bool all_resolved = !live_skin->bones().empty();
    for (auto* b : live_skin->bones()) all_resolved &= b != nullptr;
    expect(all_resolved, "bind: ...with none left unbound");

    const std::vector<coopa::gfx::engine::data::Vertex> rest = live_skin->skinned_vertices();
    require(!rest.empty(), "bind: the bind pose is skinned");
    // Pose the LIVE bone, as an Animator does -- the document's rest pose is the bind pose, so
    // rebuilding the scene with the bone already turned would just bind it in the new pose.
    auto* live_bone = app.sync().live(bone1);
    require(live_bone != nullptr && live_bone->get_transform() != nullptr, "bind: the rig's middle joint is live");
    live_bone->get_transform()->transform().set_rotation({60, 0, 0});
    tick(engine, 4);
    const std::vector<coopa::gfx::engine::data::Vertex> bent = live_skin->skinned_vertices();
    require(bent.size() == rest.size(), "bind: the same vertices, posed");
    dump(engine, "bind_to_rig_bend");   // EDITOR_DUMP_DIR=<dir> to look at the bend

    float moved_near_base = 0.0f, moved_at_tip = 0.0f;
    for (size_t i = 0; i < rest.size(); ++i) {
        const float d = glm::length(bent[i].position - rest[i].position);
        if (rest[i].position.z < 0.3f) moved_near_base = std::max(moved_near_base, d);
        if (rest[i].position.z > 2.7f) moved_at_tip = std::max(moved_at_tip, d);
    }
    expect(moved_at_tip > 0.5f, "bend: rotating the middle joint swings the far end of the mesh (" +
                                    std::to_string(moved_at_tip) + " m)");
    expect(moved_near_base < moved_at_tip * 0.2f,
           "bend: while the end over the first bone barely stirs (" + std::to_string(moved_near_base) + " m)");
}

/**
 * @brief The bind refuses what it cannot do, instead of writing something wrong: a mesh with no
 *        rig above it, and any bind at all while Pose Mode has the rig moved off its rest pose.
 */
COOPA_TEST(a_bind_refuses_a_mesh_with_no_rig_and_a_posed_rig) {
    EditorSession session({.prepare = [](Project& project) {
        coopa::yaml::save_document(project.assets() / "meshes" / "limb.yaml",
                                   mesh_to_node(tube_mesh(0.3f, 2.0f, 8, 5)));
    }});
    auto& engine = session.engine;
    auto& app = session.app;
    auto& doc = app.document();

    // The scene's own cube: a mesh at the root with nothing above it to deform it.
    const ObjectId cube = object_named(app, "cube");
    doc.select(cube);
    ObjectId skin = 0, rig = 0;
    expect(!app.bind_targets(skin, rig), "bind: a mesh with no rig above it offers no bind");
    expect(!app.bind_selection_to_rig(BindWeights::Automatic), "bind: ...so the command does nothing");

    const ObjectId rig_root = doc.add_object(doc.make_object("Rig"));
    const ObjectId bone = doc.add_object(doc.make_object("Bone"), rig_root);
    doc.set_transform(bone, {0, 0, 1.0f}, {}, {1, 1, 1}, "rest");
    const ObjectId limb = doc.add_object(doc.make_object("Limb"), rig_root);
    Node renderer = default_component("MeshRenderer");
    renderer["mesh_path"] = Node(std::string("limb"));
    doc.add_component(limb, renderer);
    app.sync().rebuild(engine, doc);
    tick(engine, 2);

    doc.select(limb);
    expect(app.bind_targets(skin, rig) && skin == limb && rig == rig_root,
           "bind: the mesh's parent is the rig when nothing above it has an Animator");

    // Pose Mode moves the live rig off its rest pose, which is the bind pose: refuse.
    doc.select(rig_root);
    app.add_animator(rig_root);
    tick(engine, 2);
    require(app.set_interaction_mode(InteractionMode::Pose), "Pose Mode opens on the rig");
    doc.select(limb);
    expect(!app.bind_selection_to_rig(BindWeights::Automatic), "bind: refused while Pose Mode holds a pose");
    app.set_interaction_mode(InteractionMode::Object);
    tick(engine, 2);
    expect(app.bind_selection_to_rig(BindWeights::Automatic), "bind: and allowed again once it is left");
}

} // namespace toy::editor::testing
