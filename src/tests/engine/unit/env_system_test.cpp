/**
 * @file env_system_test.cpp
 * @brief `EnvSystem` (toyengine/env/env_system.h): the gather turns EnvInfluence components
 *        into the frame's field, a moving source is extrapolated past the gather's one-frame
 *        transform lag, pulses live and expire, the source cap keeps the strongest -- and the
 *        consumers react to a purely local influence with NO weather in the scene at all,
 *        which is the whole point of the layer.
 */

#include <memory>
#include <string>

#include <glm/glm.hpp>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>
#include <coopa/scene/components/transform_component.h>

#include <toyengine/env/env_system.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/particles/particle_system_runner.h>

#include "engine/support/checks.h"
#include "engine/support/particle_fixtures.h"

COOPA_TEST_SUITE("env_system");

using coopa::scene::Scene;
using coopa::scene::SceneObject;
using coopa::scene::TransformComponent;
using toy::env::EnvInfluence;
using toy::env::EnvMode;
using toy::env::EnvPulse;
using toy::env::EnvShape;
using toy::env::EnvSystem;

namespace {

/** @brief Adds an object carrying a downwash influence at `at`. */
EnvInfluence* add_downwash(Scene& scene, const std::string& name, glm::vec3 at, float radius = 8.0f) {
    auto obj = std::make_unique<SceneObject>(name);
    obj->add_component<TransformComponent>()->transform().set_position(at);
    auto* e = obj->add_component<EnvInfluence>();
    e->source.radius = radius;
    e->source.falloff = 0.4f;
    e->source.shape = EnvShape::Sphere;
    e->source.mode = EnvMode::Radial;
    e->source.outward = 14.0f;
    e->source.vertical = -18.0f;
    scene.add_root_object(std::move(obj));
    return e;
}

void step(Scene& scene, int frames, float dt = 1.0f / 60.0f) {
    for (int i = 0; i < frames; ++i) {
        scene.update(dt);
        scene.late_update(dt);
    }
}

} // namespace

COOPA_TEST(the_gather_turns_components_into_the_field) {
    Scene scene("env");
    EnvInfluence* e = add_downwash(scene, "rotor", glm::vec3(0.0f, 0.0f, 4.0f));
    scene.start();
    EnvSystem* sys = toy::env::install_env_system(scene);
    require(sys != nullptr, "env: the system installs");
    expect(toy::env::install_env_system(scene) == sys, "env: installing twice returns the same system");

    step(scene, 2);
    require(sys->field().count == 1, "env: the influence became one source in the field");
    expect(sys->field().source[0].radius == 8.0f, "env: ...with its authored radius");
    expect(glm::length(toy::env::wind_at(sys->field(), glm::vec3(3.0f, 0.0f, 4.0f))) > 1.0f,
           "env: and it moves air, with no weather in the scene");

    // A disabled or zero-strength influence costs nothing and must not occupy a slot.
    e->enabled = false;
    step(scene, 1);
    expect(sys->field().count == 0, "env: a disabled influence contributes no source");
    e->enabled = true;
    e->strength = 0.0f;
    step(scene, 1);
    expect(sys->field().count == 0, "env: strength 0 contributes no source either");

    // strength scales the channels rather than the shape.
    e->strength = 0.5f;
    step(scene, 1);
    require(sys->field().count == 1, "env: it is back");
    expect_near(sys->field().source[0].outward, 7.0f, 1e-4f, "env: strength halves the channels");
    expect(sys->field().source[0].radius == 8.0f, "env: ...and leaves the shape alone");
}

COOPA_TEST(an_override_source_counts_even_with_every_channel_at_zero) {
    Scene scene("env");
    auto obj = std::make_unique<SceneObject>("courtyard");
    obj->add_component<TransformComponent>()->transform().set_position(glm::vec3(0.0f));
    auto* e = obj->add_component<EnvInfluence>();
    e->source.radius = 6.0f;
    e->source.falloff = 0.0f;
    e->source.blend = toy::env::EnvBlend::Override;
    e->source.mode = EnvMode::Directional;
    e->source.wind = glm::vec3(0.0f);     // "no wind here"
    scene.add_root_object(std::move(obj));
    scene.start();
    EnvSystem* sys = toy::env::install_env_system(scene);
    step(scene, 2);

    expect(sys->field().count == 1,
           "env: a shelter is 'zero wind here' -- an Override must be gathered even though every "
           "channel is zero, or the whole feature silently does nothing");
    toy::env::EnvField f = sys->field();
    f.wind = glm::vec3(9.0f, 0.0f, 0.0f);   // as if the weather were blowing
    expect_near(toy::env::wind_at(f, glm::vec3(0.0f)).x, 0.0f, 1e-4f,
                "env: ...and the weather does not reach inside it");
}

COOPA_TEST(a_moving_source_is_extrapolated_past_the_gather_lag) {
    // The gather runs at order 45, before TransformResolve (350), so a source's transform is a
    // frame behind. One frame of linear extrapolation cancels that for a steady mover.
    Scene scene("env");
    EnvInfluence* e = add_downwash(scene, "heli", glm::vec3(0.0f, 0.0f, 10.0f));
    scene.start();
    EnvSystem* sys = toy::env::install_env_system(scene);
    step(scene, 1);

    auto* tc = e->owner->get_transform();
    const float stride = 0.25f;
    for (int i = 1; i <= 4; ++i) {
        tc->transform().set_position(glm::vec3(stride * static_cast<float>(i), 0.0f, 10.0f));
        step(scene, 1);
    }
    // The gather saw x = 4 * stride and extrapolated one more stride ahead of it.
    expect_near(sys->field().source[0].position.x, stride * 5.0f, 1e-4f,
                "env: a steady mover's source is extrapolated one frame, so it lands where the "
                "object will be drawn rather than a frame behind it");
}

COOPA_TEST(pulses_live_and_expire) {
    Scene scene("env");
    scene.start();
    EnvSystem* sys = toy::env::install_env_system(scene);

    EnvPulse p;
    p.source.position = glm::vec3(0.0f);
    p.source.radius = 3.0f;
    p.source.mode = EnvMode::Radial;
    p.source.outward = 12.0f;
    p.attack = 0.05f;
    p.hold = 0.05f;
    p.decay = 0.2f;

    int seen = 0;
    auto conn = sys->on_pulse.connect_scoped([&](const EnvPulse&) { ++seen; });

    expect(toy::env::emit(scene, p), "pulse: a scene with the system takes it");
    expect(seen == 1, "pulse: on_pulse fires for gameplay to hang audio on");
    expect(sys->live_pulse_count() == 1, "pulse: it is live");

    step(scene, 8);   // ~0.13 s: inside the hold
    expect(sys->field().count == 1, "pulse: it is in the field while it lasts");
    expect(glm::length(toy::env::wind_at(sys->field(), glm::vec3(1.0f, 0.0f, 0.0f))) > 1.0f,
           "pulse: and it moves air");

    step(scene, 30);  // past attack + hold + decay
    expect(sys->live_pulse_count() == 0, "pulse: it is dropped once spent");
    expect(sys->field().count == 0, "pulse: and leaves the field");

    // A pulse with no lifetime would never be sampled; it must not queue forever.
    EnvPulse dud = p;
    dud.attack = dud.hold = dud.decay = 0.0f;
    toy::env::emit(scene, dud);
    expect(sys->live_pulse_count() == 0, "pulse: a zero-lifetime pulse is rejected, not queued");

    Scene bare("no_env");
    bare.start();
    expect(!toy::env::emit(bare, p), "pulse: a scene with no EnvSystem reports that nothing took it");
}

COOPA_TEST(the_source_cap_keeps_the_strongest) {
    Scene scene("env");
    // One more than the field can hold, each weaker than the last.
    for (int i = 0; i < toy::env::kMaxEnvSources + 3; ++i) {
        EnvInfluence* e = add_downwash(scene, "rotor" + std::to_string(i),
                                       glm::vec3(static_cast<float>(i) * 50.0f, 0.0f, 4.0f));
        e->source.outward = 20.0f - static_cast<float>(i);
        e->source.vertical = 0.0f;
    }
    scene.start();
    EnvSystem* sys = toy::env::install_env_system(scene);
    step(scene, 2);

    expect(sys->field().count == toy::env::kMaxEnvSources, "env: the field fills to its cap");
    expect(sys->dropped_last_frame() == 3, "env: and reports what it dropped rather than hiding it");
    // The strongest survived: nothing kept is weaker than something cut.
    float weakest_kept = 1e9f;
    for (int i = 0; i < sys->field().count; ++i)
        weakest_kept = std::min(weakest_kept, sys->field().source[i].outward);
    expect(weakest_kept > 20.0f - static_cast<float>(toy::env::kMaxEnvSources),
           "env: the cap keeps the strongest sources, not the first ones gathered");
}

COOPA_TEST(particles_are_carried_by_a_local_source_with_no_weather) {
    // The gate this guards: particles used to require a non-zero WEATHER wind before they
    // looked at the field at all, so a rotor in a weatherless scene moved nothing.
    using namespace toy::particles;
    using toy::test::add_system;
    toy::test::PScene ps;
    auto setup = [](float influence) {
        return [influence](ParticleSystem& s) {
            s.settings.shape.type = EmitShape::Point;
            s.settings.start_speed = 0.0f;
            s.settings.gravity = 0.0f;
            s.settings.rate = 0.0f;
            s.settings.start_lifetime = 100.0f;
            s.settings.wind_influence = influence;
            s.settings.looping = false;
            s.settings.duration = 100.0f;
            s.settings.bursts.push_back({0.0f, Range(10.0f), 1, 1.0f, 1.0f});
        };
    };
    // Two emitters on opposite sides of a downwash, and one far outside it.
    auto* east = add_system(ps, "east", glm::vec3(4.0f, 0.0f, 0.0f), setup(1.0f));
    auto* west = add_system(ps, "west", glm::vec3(-4.0f, 0.0f, 0.0f), setup(1.0f));
    auto* away = add_system(ps, "away", glm::vec3(60.0f, 0.0f, 0.0f), setup(1.0f));

    add_downwash(*ps.scene, "rotor", glm::vec3(0.0f, 0.0f, 0.0f), 10.0f);
    toy::test::start(ps);
    toy::env::install_env_system(*ps.scene);
    toy::test::run(ps, 4.0f);

    auto mean_vx = [](const ParticleSystem* s) {
        float v = 0.0f;
        for (std::size_t i = 0; i < s->pool().size(); ++i) v += s->pool().vel[i].x;
        return s->pool().size() ? v / static_cast<float>(s->pool().size()) : 0.0f;
    };
    expect(east->particle_count() == 10, "env particles: the burst is alive");
    expect(mean_vx(east) > 1.0f,
           "env particles: a local source carries particles east of it eastward, with NO weather "
           "in the scene");
    expect(mean_vx(west) < -1.0f, "env particles: ...and west of it westward -- it fans out");
    expect_near(mean_vx(away), 0.0f, 1e-6f, "env particles: outside the shape nothing moves");
}
