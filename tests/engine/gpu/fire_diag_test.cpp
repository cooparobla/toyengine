#include <coopa/testing/test.h>
#include <cstdio>
#include <glm/glm.hpp>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <toyengine/core/engine.h>
#include <toyengine/fire/fire_system.h>
#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"
COOPA_TEST_SUITE("fire_diag");
using namespace toy::test;
using coopa::gfx::engine::components::MeshRenderer;
static MeshRenderer* first_mr(coopa::scene::SceneObject* o) {
    if (!o) return nullptr;
    if (auto* mr = o->get_component<MeshRenderer>()) return mr;
    for (auto& c : o->children()) if (auto* r = first_mr(c.get())) return r;
    return nullptr;
}
COOPA_TEST(diag) {
    ScopedEnv fixed_dt("FIXED_DT", "0.016666667");
    ScopedEnv no_input("NO_INPUT", "1");
    toy::core::AppConfig config = make_test_config("assets/scenes/effects/fire_demo/scene.yaml", 640, 360, 640, 360);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 20);
    auto* fs = toy::fire::find_fire(engine.scene());
    auto* a = engine.scene().find_object("chair_a");
    auto* b = a->get_component<toy::fire::Burnable>();
    auto* mr = first_mr(a);
    b->ignite();
    for (int i = 0; i < 8; ++i) {
        tick_frames(engine, 120);
        std::printf("DIAG f%4d front=%.3f burning=%d | mat.burning=%d params=(%.2f,%.2f,%.2f,%.3f)\n",
                    (i+1)*120, b->front(), b->burning() ? 1 : 0,
                    mr->material.burning ? 1 : 0, mr->material.shader_params.x,
                    mr->material.shader_params.y, mr->material.shader_params.z,
                    mr->material.shader_params.w);
    }
    expect(true, "diag");
}
