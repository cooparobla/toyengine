/**
 * @file rig_render_test.cpp
 * @brief Rigs in the engine: a vertex-group skinned mesh follows an animated bone from a clip file,
 *        animation_test's bones, skin and IK (look-at, two-bone) all move, and GPU skinning matches
 *        the CPU path vertex for vertex and pixel for pixel.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>
#include <coopa/animation/ik.h>
#include <toyengine/scene/skinned_mesh_renderer.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("rig_render");

using namespace toy::test;

/** @brief A rig end to end in the engine: an Animator auto-plays a clip FILE on a bone of an
 *         object hierarchy, and a mesh whose vertex groups name the bones (no `bones:`, no inverse
 *         bind matrices -- what the editor's Weight Paint writes) follows it. */
COOPA_TEST(skinned_mesh_follows_an_animated_bone) {
    namespace fs = std::filesystem;
    const fs::path dir = coopa::test::scratch_dir("rig");
    fs::remove_all(dir);
    fs::create_directories(dir / "meshes");
    fs::create_directories(dir / "animations" / "Rig");
    {
        std::ofstream(dir / "meshes" / "strip.yaml") <<
            "vertices: [[-0.5, 0, 0], [0.5, 0, 0], [0.5, 0, 2], [-0.5, 0, 2]]\n"
            "normals: [[0, -1, 0], [0, -1, 0], [0, -1, 0], [0, -1, 0]]\n"
            "uvs: [[0, 0], [1, 0], [1, 1], [0, 1]]\n"
            "faces: [[0, 1, 2, 3]]\n"
            "colors: []\n"
            "weights:\n  - {Lower: 1.0}\n  - {Lower: 1.0}\n  - {Upper: 1.0}\n  - {Upper: 1.0}\n";
        std::ofstream(dir / "animations" / "Rig" / "sway.yaml") <<
            "clip:\n  name: sway\n  wrap: once\n  length: 0.5\n  tracks:\n"
            "    - object: Lower/Upper\n      property: position\n      keys:\n"
            "        - {time: 0.0, value: [0, 0, 1]}\n        - {time: 0.5, value: [1, 0, 1]}\n";
        std::ofstream(dir / "scene.yaml") <<
            "format: blender\nscene:\n  scene_name: RigTest\n  root_objects:\n"
            "    - name: Camera\n      components:\n        - type: Transform\n          position: {x: 0, y: -6, z: 1}\n"
            "          rotation: {x: 90, y: 0, z: 0}\n        - type: Camera\n          main: true\n"
            "    - name: Rig\n      components:\n        - type: Transform\n"
            "        - type: Animator\n          auto_play: sway\n          states:\n"
            "            - {name: sway, clip: animations/Rig/sway.yaml}\n"
            "      children:\n"
            "        - name: Lower\n          components:\n            - type: Transform\n"
            "          children:\n"
            "            - name: Upper\n              components:\n                - type: Transform\n"
            "                  position: {x: 0, y: 0, z: 1}\n"
            "        - name: Skin\n          components:\n            - type: Transform\n"
            "            - type: MeshRenderer\n              material: {albedo: {r: 0.8, g: 0.6, b: 0.4}}\n"
            "            - type: SkinnedMeshRenderer\n              mesh_path: strip\n";
    }
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config((dir / "scene.yaml").string(), 320, 180, 160, 90));
    tick_frames(engine, 3);
    auto* skin_obj = engine.scene().find_object("Skin");
    auto* smr = skin_obj ? skin_obj->get_component<toy::scene::SkinnedMeshRenderer>() : nullptr;
    expect(smr && smr->is_ready(), "rig: the skinned mesh built its GPU mesh");
    if (!smr || !smr->is_ready()) return;
    expect(smr->rig() && smr->rig()->name() == "Rig", "rig: the rig root is the nearest Animator up the hierarchy");
    expect(smr->bones().size() == 2 && smr->bones()[0] && smr->bones()[1] && smr->bones()[1]->name() == "Upper",
           "rig: the vertex groups name the bones, found under the rig");
    tick_frames(engine, 45);   // past the clip's end: Upper rests at x = 1
    float top_x = 0.0f, bottom_x = 0.0f;
    for (const auto& v : smr->skinned_vertices()) {
        if (v.position.z > 1.5f) top_x += v.position.x; else bottom_x += v.position.x;
    }
    // The quad's triangle corners: top (0.5, 0.5, -0.5) and bottom (-0.5, 0.5, -0.5) -- bind
    // means of +1/6 and -1/6 in x.
    const float n = static_cast<float>(smr->skinned_vertices().size()) / 2.0f;
    expect(std::abs(top_x / n - (1.0f / 6.0f + 1.0f)) < 0.02f,
           "rig: the vertices weighted to the animated bone moved with it, 1 m (mean x " + std::to_string(top_x / n) + ")");
    expect(std::abs(bottom_x / n + 1.0f / 6.0f) < 0.02f,
           "rig: the vertices weighted to the still bone stayed (bind pose from the rest pose)");
    fs::remove_all(dir);
}

/** @brief The animation_test scene runs: the arm's joints, the skinned tentacle and the
 *         self-animating ball all move, from their clip files. */
COOPA_TEST(animation_test_bones_skin_and_ik_move) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config("assets/scenes/animation_test/scene.yaml", 640, 360, 320, 180));
    tick_frames(engine, 2);
    auto& scene = engine.scene();
    auto* elbow = scene.find_object("elbow");
    auto* ball = scene.find_object("ball");
    auto* skin = scene.find_object("tentacle_skin");
    auto* smr = skin ? skin->get_component<toy::scene::SkinnedMeshRenderer>() : nullptr;
    expect(elbow && ball && smr, "animation_test: the rigs loaded");
    if (!elbow || !ball || !smr) return;
    float min_z = 1e9f, max_z = -1e9f, min_scale_z = 1e9f;
    glm::quat q0 = elbow->get_transform()->transform().rotation_quat();
    float max_turn = 0.0f, max_tip_x = 0.0f;
    for (int i = 0; i < 90; ++i) {
        tick_frames(engine, 1);
        const auto& bt = ball->get_transform()->transform();
        min_z = std::min(min_z, bt.position().z);
        max_z = std::max(max_z, bt.position().z);
        min_scale_z = std::min(min_scale_z, bt.scale().z);
        max_turn = std::max(max_turn, 1.0f - std::abs(glm::dot(q0, elbow->get_transform()->transform().rotation_quat())));
        for (const auto& v : smr->skinned_vertices()) if (v.position.z > 2.0f) max_tip_x = std::max(max_tip_x, std::abs(v.position.x));
    }
    expect(max_z - min_z > 1.2f, "animation_test: the ball bounces (z " + std::to_string(min_z) + " .. " + std::to_string(max_z) + ")");
    expect(min_scale_z < 0.45f, "animation_test: ...and squashes on landing (scale z " + std::to_string(min_scale_z) + ")");
    expect(max_turn > 0.05f, "animation_test: the arm's elbow bends");
    expect(smr->is_ready() && smr->bones().size() == 4 && max_tip_x > 0.4f,
           "animation_test: the tentacle's skin follows its bones (tip x " + std::to_string(max_tip_x) + ")");

    // IK: the watcher's head follows its orbiting target (LookAtIK), the reacher's hand stays on
    // its drifting target (TwoBoneIK).
    auto* head = scene.find_object("head");
    auto* look_target = scene.find_object("look_target");
    auto* hand = scene.find_object("reach_hand");
    auto* reach_target = scene.find_object("reach_target");
    expect(head && look_target && hand && reach_target, "animation_test: the IK rigs loaded");
    if (!head || !look_target || !hand || !reach_target) return;
    auto pos = [](coopa::scene::SceneObject* o) { return glm::vec3(o->get_transform()->get_world_matrix()[3]); };
    float max_gaze_err = 0.0f, max_reach_err = 0.0f, max_head_turn = 0.0f;
    const glm::vec3 fwd0 = coopa::anim::ik::rotation_of(head->get_transform()->get_world_matrix()) * glm::vec3(0, -1, 0);
    for (int i = 0; i < 120; ++i) {
        tick_frames(engine, 1);
        const glm::vec3 fwd = coopa::anim::ik::rotation_of(head->get_transform()->get_world_matrix()) * glm::vec3(0, -1, 0);
        const glm::vec3 to = glm::normalize(pos(look_target) - pos(head));
        max_gaze_err = std::max(max_gaze_err, glm::degrees(std::acos(std::clamp(glm::dot(fwd, to), -1.0f, 1.0f))));
        max_head_turn = std::max(max_head_turn, glm::degrees(std::acos(std::clamp(glm::dot(fwd, fwd0), -1.0f, 1.0f))));
        max_reach_err = std::max(max_reach_err, glm::length(pos(hand) - pos(reach_target)));
    }
    expect(max_head_turn > 30.0f, "animation_test: the watcher's head turns (" + std::to_string(max_head_turn) + " deg)");
    expect(max_gaze_err < 25.0f, "animation_test: ...toward its target (worst lag " + std::to_string(max_gaze_err) + " deg)");
    expect(max_reach_err < 0.02f, "animation_test: the reacher's hand stays on its target (worst " + std::to_string(max_reach_err) + " m)");
}

/** @brief GPU skinning (the compute pre-pass) against the CPU fallback: the same animated frame
 *         of animation_test renders the same with `skinning: gpu` and `skinning: cpu`, and the
 *         vertices the dispatch wrote match the CPU skin of the same palette. */
COOPA_TEST(gpu_skinning_matches_cpu) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    struct Run { Frame frame; bool gpu = false; float max_err = -1.0f; float moved = 0.0f; size_t verts = 0; };
    auto run = [](const std::string& mode) {
        Run r;
        toy::core::AppConfig cfg = make_test_config("assets/scenes/animation_test/scene.yaml", 640, 360, 320, 180);
        cfg.render.skinning = mode;
        toy::core::Engine engine(std::move(cfg));
        tick_frames(engine, 2);
        auto* skin = engine.scene().find_object("tentacle_skin");
        auto* smr = skin ? skin->get_component<toy::scene::SkinnedMeshRenderer>() : nullptr;
        std::vector<coopa::gfx::engine::data::Vertex> early;
        if (smr && smr->is_ready()) early = smr->skinned_vertices();
        tick_frames(engine, 38);
        if (smr && smr->is_ready()) {
            r.gpu = smr->gpu_skinned();
            engine.device().wait_idle();
            const auto& mesh = smr->mesh();
            const auto& cpu = smr->skinned_vertices();
            r.verts = cpu.size();
            std::vector<coopa::gfx::engine::data::Vertex> gpu(cpu.size());
            mesh->vertex_buffer(mesh->active_slot()).download(gpu.data(), sizeof(gpu[0]) * gpu.size());
            r.max_err = 0.0f;
            for (size_t i = 0; i < cpu.size(); ++i) {
                r.max_err = std::max(r.max_err, glm::length(gpu[i].position - cpu[i].position));
                r.max_err = std::max(r.max_err, glm::length(gpu[i].normal - cpu[i].normal));
                r.max_err = std::max(r.max_err, std::abs(gpu[i].uv.x - cpu[i].uv.x) + std::abs(gpu[i].uv.y - cpu[i].uv.y));
                if (i < early.size()) r.moved = std::max(r.moved, glm::length(gpu[i].position - early[i].position));
            }
        }
        r.frame = engine.capture_image(/*low_res=*/true);
        return r;
    };
    const Run g = run("gpu");
    const Run c = run("cpu");
    expect(g.gpu, "gpu skinning: skinning: gpu dispatches on a compute device");
    expect(!c.gpu, "gpu skinning: skinning: cpu keeps the CPU fallback");
    expect(g.verts > 0 && g.max_err >= 0.0f && g.max_err < 1e-4f,
           "gpu skinning: the dispatch's vertices match the CPU skin of the same palette (max err " + std::to_string(g.max_err) + ")");
    expect(g.moved > 0.05f, "gpu skinning: ...and they are animated, not the bind pose (moved " + std::to_string(g.moved) + " m)");
    expect(same_extent(g.frame, c.frame), "gpu skinning: both captures share one extent");
    if (!same_extent(g.frame, c.frame)) return;
    const long long diff = count_diff(g.frame, c.frame, 6);
    const long long budget = static_cast<long long>(g.frame.width) * g.frame.height / 200;   // 0.5%
    expect(diff <= budget, "gpu skinning: GPU and CPU captures of the same frame agree (" + std::to_string(diff) + " px differ)");
    if (diff > budget) { dump_frame(g.frame, "skinning_gpu"); dump_frame(c.frame, "skinning_cpu"); }
}
