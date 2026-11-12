/**
 * @file module_hooks_test.cpp
 * @brief Project module hooks (toyengine/core/module.h): each one fires, in the order the
 *        engine promises, and a project can replace a built-in scene system from
 *        on_prepare_scene.
 *
 * These are the extension points that let a project add what the engine deliberately does not
 * model -- its own scene systems, its own surface shaders, its own world shape -- without
 * forking the engine. Nothing else asserts their contract, and every one of them fails
 * SILENTLY when it regresses: a hook that stops firing just means the project's feature quietly
 * does not exist. Hence a test per hook.
 *
 * **The probe module is armed, not always-on.** `toy::core::modules()` is process-global and
 * this file is linked into the whole runner, so its static registration happens no matter which
 * suite was selected -- and an unguarded hook that removed a system or added a shader would
 * then do it to every Engine every other suite builds. Each test arms the probe for the
 * Engine it is about to construct and disarms it after.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <string>
#include <vector>

#include <coopa/scene/scene.h>
#include <toyengine/core/engine.h>
#include <toyengine/core/module.h>
#include <toyengine/env/world_frame.h>
#include <toyengine/render/toy_render_config.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("module_hooks");

using namespace toy::test;

namespace {

/// What the hooks recorded. Only written while `armed`.
struct HookLog {
    bool armed = false;
    int engine_init = 0;
    int prepare_scene = 0;
    int render_config = 0;
    std::vector<std::string> order;              ///< Hook names, in the order they ran.
    coopa::scene::Scene* last_scene = nullptr;
    bool saw_terrain_system_installed = false;   ///< prepare_scene runs after the built-ins.
    bool removed_terrain_system = false;
    bool remove_a_system = false;                ///< Only the replace test asks for this.
};

HookLog& log() {
    static HookLog l;
    return l;
}

/// Arms the probe for one Engine, and disarms it however the test leaves.
struct Armed {
    explicit Armed(bool remove_a_system = false) {
        log() = HookLog{};
        log().armed = true;
        log().remove_a_system = remove_a_system;
    }
    ~Armed() { log().armed = false; }
};

/// A surface shader the engine does not know about, added through on_render_config.
const char* k_project_shader = "module_hooks_probe";

// Registered the way a project's src/ does it -- a static initializer, so this runs before
// main() and therefore before any Engine exists. All three hooks on one module: TOY_MODULE()
// only fills on_engine_init, so a module wanting the other two is written out longhand.
const bool k_registered = toy::core::register_module({
    "module_hooks_probe",
    /* on_engine_init */
    [](toy::core::Engine&) {
        if (!log().armed) return;
        ++log().engine_init;
        log().order.push_back("engine_init");
    },
    /* on_prepare_scene */
    [](toy::core::Engine&, coopa::scene::Scene& scene) {
        if (!log().armed) return;
        ++log().prepare_scene;
        log().order.push_back("prepare_scene");
        log().last_scene = &scene;
        // Every built-in system is installed by now: that is the whole point of this hook's
        // placement, and what lets a project replace one rather than race it.
        log().saw_terrain_system_installed = scene.find_system("Terrain") != nullptr;
        if (log().remove_a_system && log().saw_terrain_system_installed) {
            scene.remove_system("Terrain");
            log().removed_terrain_system = scene.find_system("Terrain") == nullptr;
        }
    },
    /* on_render_config */
    [](toy::render::ToyRenderConfig& rc) {
        if (!log().armed) return;
        ++log().render_config;
        log().order.push_back("render_config");
        // Borrowing an existing pair of entry points: this test is about whether the hook's
        // result reaches the pipeline, not about the shader itself.
        coopa::gfx::pipeline::SurfaceShaderDesc d;
        d.name = k_project_shader;
        d.domain = coopa::gfx::pipeline::SurfaceShaderDomain::Opaque;
        d.vert = "terrain_styled.vert";
        d.frag = "terrain_styled.frag";
        rc.surface_shaders.add(d);
    },
});

const char* k_scene = "assets/scenes/rendering/sky_demo/scene.yaml";

} // namespace

COOPA_TEST(every_hook_fires_and_render_config_runs_before_engine_init) {
    expect(k_registered, "the probe module registered");
    ScopedEnv no_input("NO_INPUT", "1");
    Armed armed;
    toy::core::Engine engine(make_test_config(k_scene, 320, 180, 160, 90));

    const HookLog& l = log();
    // Once per config DERIVATION, not once per Engine: a scene carrying render overrides
    // makes a second config. Each starts from a fresh copy, so the hook stays safe to
    // repeat even though SurfaceShaderRegistry::add() throws on a duplicate name.
    expect(l.render_config >= 1, "on_render_config fired");
    expect(l.engine_init == 1, "on_engine_init fired once");
    expect(l.prepare_scene >= 1, "on_prepare_scene fired");

    // The documented order, and the reason on_render_config cannot be installed from
    // on_engine_init: make_render_config_() runs in the member-init list, ahead of the
    // constructor body.
    const auto ri = std::find(l.order.begin(), l.order.end(), "render_config");
    const auto ei = std::find(l.order.begin(), l.order.end(), "engine_init");
    const auto pi = std::find(l.order.begin(), l.order.end(), "prepare_scene");
    require(ri != l.order.end() && ei != l.order.end() && pi != l.order.end(),
            "all three hooks appear in the order log");
    expect(ri < ei, "on_render_config runs before on_engine_init");
    expect(ei < pi, "on_engine_init runs before on_prepare_scene");
}

COOPA_TEST(a_project_surface_shader_reaches_the_pipeline) {
    ScopedEnv no_input("NO_INPUT", "1");
    Armed armed;
    toy::core::Engine engine(make_test_config(k_scene, 320, 180, 160, 90));
    // The registry the pipeline built its variants from. A shader added any later than
    // on_render_config would be absent here and would silently never draw.
    expect(engine.render_config().surface_shaders.find(k_project_shader) != nullptr,
           "the project's surface shader is in the live registry");
}

COOPA_TEST(on_prepare_scene_sees_the_built_in_systems_and_can_replace_one) {
    ScopedEnv no_input("NO_INPUT", "1");
    Armed armed(/*remove_a_system=*/true);
    toy::core::Engine engine(make_test_config("assets/scenes/terrain/terrain_demo/scene.yaml",
                                              320, 180, 160, 90));
    const HookLog& l = log();
    expect(l.saw_terrain_system_installed,
           "on_prepare_scene runs after the engine installed its own systems");
    expect(l.removed_terrain_system,
           "a project can remove_system() a built-in it replaces");
    expect(l.last_scene != nullptr, "the hook received the scene being prepared");
}

COOPA_TEST(no_world_frame_source_means_the_flat_world) {
    // The engine models a flat world and nothing else: with no source installed, every query
    // reduces to what it has always been. This is the property the whole seam rests on.
    const toy::env::WorldFrame flat{};
    expect(!flat.curved(), "a default frame is not curved");
    EXPECT_VEC_NEAR(flat.up_at(glm::vec3(5.0f, -3.0f, 12.0f)), toy::env::k_world_up, 1e-6f);
    EXPECT_NEAR(flat.altitude_at(glm::vec3(5.0f, -3.0f, 12.0f)), 12.0f, 1e-6f);
}
