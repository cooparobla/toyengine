/**
 * @file character_cloth_render_test.cpp
 * @brief Character cloth end to end: a painted banner on an animated rig simulates (pinned row
 *        rides the bone exactly, free rows drape and never exceed their painted leash), the
 *        blended mesh keeps re-uploading, and the GPU deform path matches the CPU fallback
 *        vertex for vertex and pixel for pixel (the rig_render parity recipe).
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <physxcoopa/cloth/cloth_collision.h>
#include <toyengine/cloth/character_cloth.h>
#include <toyengine/cloth/cloth_mesh_system.h>
#include <toyengine/core/engine.h>
#include <toyengine/rig/rig_component.h>
#include <toyengine/scene/skinned_mesh_renderer.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("character_cloth_render");

using namespace toy::test;

namespace {

/// Grid resolution of the banner (res x res welded vertices).
constexpr int k_res = 7;

/**
 * @brief Writes the test world: a one-bone rig whose clip sways it sideways, and a hanging
 *        banner mesh fully weighted to that bone, painted 0 max_distance on its top row (rides
 *        the bone) ramping up toward the hem (free cloth).
 */
std::filesystem::path write_banner_world() {
    namespace fs = std::filesystem;
    const fs::path dir = coopa::test::scratch_dir("character_cloth");
    fs::remove_all(dir);
    fs::create_directories(dir / "meshes");
    fs::create_directories(dir / "rigs");
    fs::create_directories(dir / "animations" / "Rig");

    // The banner hangs from z = 2 down to z = 0, 2 m wide, in the XZ plane.
    std::ostringstream mesh;
    mesh << "vertices: [";
    for (int r = 0; r < k_res; ++r) {           // r = 0 is the TOP row
        for (int c = 0; c < k_res; ++c) {
            const float x = -1.0f + 2.0f * static_cast<float>(c) / (k_res - 1);
            const float z = 2.0f - 2.0f * static_cast<float>(r) / (k_res - 1);
            mesh << (r + c ? ", " : "") << "[" << x << ", 0, " << z << "]";
        }
    }
    mesh << "]\nnormals: [";
    for (int i = 0; i < k_res * k_res; ++i) mesh << (i ? ", " : "") << "[0, -1, 0]";
    mesh << "]\nuvs: [";
    for (int r = 0; r < k_res; ++r) {
        for (int c = 0; c < k_res; ++c) {
            mesh << (r + c ? ", " : "") << "[" << static_cast<float>(c) / (k_res - 1) << ", "
                 << static_cast<float>(r) / (k_res - 1) << "]";
        }
    }
    mesh << "]\nfaces: [";
    bool first = true;
    for (int r = 0; r + 1 < k_res; ++r) {
        for (int c = 0; c + 1 < k_res; ++c) {
            const int i00 = r * k_res + c, i10 = i00 + 1;
            const int i01 = i00 + k_res, i11 = i01 + 1;
            mesh << (first ? "" : ", ") << "[" << i00 << ", " << i10 << ", " << i11 << ", "
                 << i01 << "]";
            first = false;
        }
    }
    mesh << "]\nweights:\n";
    for (int i = 0; i < k_res * k_res; ++i) mesh << "  - {Hang: 1.0}\n";
    mesh << "cloth:\n  max_distance: [";
    for (int r = 0; r < k_res; ++r) {
        for (int c = 0; c < k_res; ++c) {
            mesh << (r + c ? ", " : "") << (r == 0 ? 0.0f : 0.25f * static_cast<float>(r));
        }
    }
    // No anim_drive: the free rows must behave as pure cloth so the hem-trails assertion means
    // something (a stiff drive tracks a moving target with near-zero steady-state lag -- that
    // behaviour is covered by the solver's own skin_anim_drive test).
    mesh << "]\n";
    std::ofstream(dir / "meshes" / "banner.yaml") << mesh.str();

    std::ofstream(dir / "rigs" / "hang.yaml") <<
        "rig:\n  name: hang\n  bones:\n"
        "    - {name: Hang, head: {x: 0, y: 0, z: 2}, tail: {x: 0, y: 0, z: 3}}\n";
    std::ofstream(dir / "animations" / "Rig" / "sway.yaml") <<
        "clip:\n  name: sway\n  wrap: loop\n  length: 2.0\n  bones:\n"
        "    - bone: Hang\n      location:\n"
        "        - {time: 0.0, value: [0, 0, 0]}\n"
        "        - {time: 1.0, value: [0.8, 0, 0]}\n"
        "        - {time: 2.0, value: [0, 0, 0]}\n";
    std::ofstream(dir / "scene.yaml") <<
        "scene:\n  scene_name: CharacterClothTest\n  root_objects:\n"
        "    - name: Camera\n      components:\n        - type: Transform\n          position: {x: 0, y: -6, z: 1}\n"
        "          rotation: {x: 90, y: 0, z: 0}\n        - type: Camera\n          main: true\n"
        "    - name: Sun\n      components:\n        - type: Transform\n"
        "        - type: DirectionalLight\n          direction: {x: -0.4, y: 0.4, z: -0.8}\n"
        "    - name: Rig\n      components:\n        - type: Transform\n"
        "        - type: Rig\n          rig: rigs/hang.yaml\n"
        "        - type: SkeletalAnimator\n          auto_play: sway\n          states:\n"
        "            - {name: sway, clip: animations/Rig/sway.yaml}\n"
        "      children:\n"
        "        - name: Banner\n          components:\n            - type: Transform\n"
        "            - type: MeshRenderer\n              material: {albedo: {r: 0.7, g: 0.2, b: 0.3}, cull_backfaces: false}\n"
        "            - type: SkinnedMeshRenderer\n              mesh_path: banner\n"
        "            - type: CharacterCloth\n              damping: 0.1\n"
        "              anim_drive_compliance: 0.001\n"
        // A banner on a pole has no body: the auto per-bone colliders a GARMENT wants
        // would wrap this rig's single bone and push the sheet off its own pinned row.
        "              auto_colliders: false\n";
    return dir;
}

} // namespace

/** @brief The simulation: pinned row exactly on its skinned targets, free rows leashed to their
 *         painted max_distance while the bone sways, and the hem actually trails (it IS cloth,
 *         not skinning). */
COOPA_TEST(banner_simulates_pinned_row_rides_and_leash_holds) {
    namespace fs = std::filesystem;
    const fs::path dir = write_banner_world();
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(make_test_config((dir / "scene.yaml").string(), 320, 180, 160, 90));
    tick_frames(engine, 3);

    auto* system = dynamic_cast<toy::cloth::ClothMeshSystem*>(engine.scene().find_system("ClothMesh"));
    require(system != nullptr, "character cloth: the system is installed");
    const coopa::physx::cloth::Cloth* sim = system->cloth("Banner");
    require(sim != nullptr, "character cloth: the banner built its simulation");
    expect(sim->particles.size() == static_cast<size_t>(k_res * k_res),
           "character cloth: one particle per welded vertex");
    expect(!sim->tethers.empty(), "character cloth: tethers were seeded from the pinned row");

    float worst_pin = 0.0f, worst_leash = 0.0f, hem_trail = 0.0f;
    for (int i = 0; i < 150; ++i) {
        tick_frames(engine, 1);
        sim = system->cloth("Banner");
        if (sim == nullptr) break;
        for (size_t p = 0; p < sim->particles.size(); ++p) {
            const float d =
                glm::length(sim->particles[p].position - sim->skin_targets.position[p]);
            const float leash = sim->skin.max_distance[p] * sim->params.max_distance_scale;
            if (leash <= 1e-6f) worst_pin = std::max(worst_pin, d);
            else worst_leash = std::max(worst_leash, d - leash);
        }
        // Bottom-row mean lag behind its own targets, in X (the sway axis).
        float lag = 0.0f;
        const size_t hem0 = static_cast<size_t>((k_res - 1) * k_res);
        for (size_t c = 0; c < static_cast<size_t>(k_res); ++c) {
            lag += sim->skin_targets.position[hem0 + c].x - sim->particles[hem0 + c].position.x;
        }
        hem_trail = std::max(hem_trail, std::abs(lag) / k_res);
    }
    require(sim != nullptr, "character cloth: the simulation stayed alive");
    expect(sim->time > 2.0f, "character cloth: the solver advanced every frame, no rebuild churn");
    expect(worst_pin < 1e-3f,
           "character cloth: painted-zero vertices ride the bone exactly (worst " +
               std::to_string(worst_pin) + " m)");
    expect(worst_leash < 2e-3f,
           "character cloth: no vertex exceeds its painted max_distance (worst over by " +
               std::to_string(worst_leash) + " m)");
    // Measured regimes: a skin-glued hem (stiff anim drive, or no simulation at all) trails
    // ~8e-5 m; free cloth at this sway speed trails ~9e-3 m. 5 mm sits 60x above the glued
    // regime, so this cannot pass on skinning alone.
    expect(hem_trail > 0.005f,
           "character cloth: the hem trails the sway -- it is simulating, not skinning (" +
               std::to_string(hem_trail) + " m)");
    fs::remove_all(dir);
}

/**
 * @brief The demo scene's garment does not pass through the body anywhere in the walk cycle.
 *
 * The requirement a garment actually has to meet: over a full cycle, no SIMULATED particle ends a
 * frame inside any collider the solver is holding it off. Read from the system
 * (`bone_colliders()`) and projected with the solver's own `project_particle()`, so the test
 * measures the shapes the engine really uses instead of re-deriving a second guess at them --
 * which is what it did while the colliders came from a bone-length rule, and which silently went
 * stale the moment they started being fitted to the drawn parts instead.
 *
 * That those colliders cover the body a viewer SEES is a separate claim, and cloth_probe's
 * body_parts_fit_inside_their_cloth_colliders is where it is made. This test and that one are
 * the two halves of "no limb through the garment"; neither means much alone.
 *
 * Skin-driven vertices (painted max_distance 0) are excluded, and must be: they are written from
 * their skinned targets and skip collision entirely by design, so a waistband authored to hug the
 * hips legitimately sits against the pelvis capsule. Only cloth the solver is free to move is
 * cloth the solver owes a standoff.
 */
COOPA_TEST(demo_jacket_never_enters_the_body) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::Engine engine(
        make_test_config("assets/scenes/animation/character_cloth_demo/scene.yaml", 320, 180, 160, 90));
    tick_frames(engine, 5);

    auto* system = dynamic_cast<toy::cloth::ClothMeshSystem*>(engine.scene().find_system("ClothMesh"));
    require(system != nullptr, "demo: the cloth system is installed");
    auto* jacket = engine.scene().find_object("jacket");
    auto* cc = jacket ? jacket->get_component<toy::cloth::CharacterCloth>() : nullptr;
    auto* smr = jacket ? jacket->get_component<toy::scene::SkinnedMeshRenderer>() : nullptr;
    require(cc != nullptr && smr != nullptr, "demo: the garment carries CharacterCloth + SkinnedMeshRenderer");
    require(cc->auto_colliders, "demo: the garment relies on auto bone colliders");
    toy::rig::Rig* rig = smr->rig();
    require(rig != nullptr && rig->bone_count() > 10, "demo: the mannequin rig is loaded");
    require(system->cloth("jacket") != nullptr, "demo: the garment built its simulation");
    // The whole point of auto_colliders: the body is covered by its own drawn parts, not by the
    // handful of bones the scene happens to list. Without this the no-penetration check below
    // could pass simply by the garment never meeting a limb.
    expect(system->bone_collider_count("jacket") >= rig->bone_count() - 1,
           "demo: a collider per rig bone (" + std::to_string(system->bone_collider_count("jacket")) +
               " for " + std::to_string(rig->bone_count()) + " bones)");

    float worst_depth = 0.0f;
    int worst_particle = -1, worst_frame = -1;
    // 150 frames at 1/60 s covers the walk clip several times over.
    for (int frame = 0; frame < 150; ++frame) {
        tick_frames(engine, 1);
        const coopa::physx::cloth::Cloth* sim = system->cloth("jacket");
        const std::vector<coopa::physx::cloth::ClothCollider> colliders =
            system->bone_colliders("jacket");
        if (sim == nullptr || colliders.empty()) break;
        for (std::size_t i = 0; i < sim->particles.size(); ++i) {
            if (sim->particles[i].inv_mass == 0.0f) continue;   // skin-driven: see the doc
            for (const coopa::physx::cloth::ClothCollider& c : colliders) {
                glm::vec3 moved = sim->particles[i].position, normal(0.0f);
                float depth = 0.0f;
                // Zero thickness: the question is penetration, not whether the standoff held.
                if (!coopa::physx::cloth::project_particle(c, 0.0f, moved, normal, depth)) continue;
                if (depth > worst_depth) {
                    worst_depth = depth;
                    worst_particle = static_cast<int>(i);
                    worst_frame = frame;
                }
            }
        }
    }
    // NOT a tolerance that happens to pass: the solver re-sweeps a particle against the whole
    // collider set until it is clear (k_collider_passes), so the measured worst case over the
    // cycle is 0 m and this bound is slack for float noise alone. A regression that reintroduces
    // the single pass over overlapping capsules measured 4.2 cm here, so the margin is wide
    // enough to be a real signal and tight enough to catch it.
    expect(worst_depth < 0.002f,
           "demo: no garment vertex enters the body (worst " + std::to_string(worst_depth) +
               " m, particle " + std::to_string(worst_particle) + " at frame " +
               std::to_string(worst_frame) + ")");
}

/** @brief GPU deform against the CPU fallback: the same fixed-step frame of the same world,
 *         `skinning: gpu` vs `skinning: cpu`, must land the same blended vertices in the mesh's
 *         buffer and the same pixels on screen -- the mandatory parity test for any new kernel
 *         (rig_render's recipe). */
COOPA_TEST(gpu_deform_matches_cpu) {
    namespace fs = std::filesystem;
    const fs::path dir = write_banner_world();
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    struct Run {
        Frame frame;
        bool gpu = false;
        std::vector<coopa::gfx::engine::data::Vertex> verts;
        float moved = 0.0f;
    };
    auto run = [&](const std::string& mode) {
        Run r;
        toy::core::AppConfig cfg = make_test_config((dir / "scene.yaml").string(), 320, 180, 160, 90);
        cfg.render.skinning = mode;
        toy::core::Engine engine(std::move(cfg));
        tick_frames(engine, 3);
        auto* banner = engine.scene().find_object("Banner");
        auto* smr = banner ? banner->get_component<toy::scene::SkinnedMeshRenderer>() : nullptr;
        std::vector<coopa::gfx::engine::data::Vertex> early;
        if (smr && smr->is_ready()) {
            engine.device().wait_idle();
            early.resize(smr->source()->vertices.size());
            smr->mesh()->vertex_buffer(smr->mesh()->active_slot())
                .download(early.data(), sizeof(early[0]) * early.size());
        }
        tick_frames(engine, 57);
        if (smr && smr->is_ready()) {
            r.gpu = smr->gpu_skinned();
            engine.device().wait_idle();
            r.verts.resize(smr->source()->vertices.size());
            smr->mesh()->vertex_buffer(smr->mesh()->active_slot())
                .download(r.verts.data(), sizeof(r.verts[0]) * r.verts.size());
            for (size_t i = 0; i < r.verts.size() && i < early.size(); ++i) {
                r.moved = std::max(r.moved, glm::length(r.verts[i].position - early[i].position));
            }
        }
        r.frame = engine.capture_image(/*low_res=*/true);
        return r;
    };
    const Run g = run("gpu");
    const Run c = run("cpu");
    require(!g.verts.empty() && g.verts.size() == c.verts.size(),
            "character cloth deform: both runs produced the banner's vertices");
    expect(g.gpu, "character cloth deform: skinning: gpu put the renderer on the compute path "
                  "(the deform dispatch's gate)");
    expect(!c.gpu, "character cloth deform: skinning: cpu kept the CPU blend");
    expect(g.moved > 0.01f, "character cloth deform: the mesh keeps deforming (moved " +
                                std::to_string(g.moved) + " m)");
    float max_err = 0.0f;
    for (size_t i = 0; i < g.verts.size(); ++i) {
        max_err = std::max(max_err, glm::length(g.verts[i].position - c.verts[i].position));
        max_err = std::max(max_err, glm::length(g.verts[i].normal - c.verts[i].normal));
    }
    expect(max_err < 1e-3f,
           "character cloth deform: GPU and CPU paths agree vertex for vertex (max err " +
               std::to_string(max_err) + ")");
    expect(same_extent(g.frame, c.frame), "character cloth deform: captures share one extent");
    if (same_extent(g.frame, c.frame)) {
        const long long diff = count_diff(g.frame, c.frame, 6);
        const long long budget = static_cast<long long>(g.frame.width) * g.frame.height / 200;
        expect(diff <= budget, "character cloth deform: GPU and CPU captures agree (" +
                                   std::to_string(diff) + " px differ)");
        if (diff > budget) {
            dump_frame(g.frame, "character_cloth_gpu");
            dump_frame(c.frame, "character_cloth_cpu");
        }
    }
    fs::remove_all(dir);
}
