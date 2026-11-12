/**
 * @file particle_yaml_test.cpp
 * @brief The ParticleSystem YAML parser reads every value form (ranges, enums, curves, gradients,
 *        bursts, sub emitters) and rejects an unknown enum instead of silently defaulting.
 */

#include <coopa/testing/test.h>

#include <string>

#include <fkYAML/node.hpp>
#include <toyengine/particles/particle_yaml.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("particle_yaml");

/** @brief The YAML parser reads every value form: ranges, enums, curves, gradients, bursts. */
COOPA_TEST(parses_every_value_form_and_rejects_unknown_enums) {
    using namespace toy::particles;
    const fkyaml::node node = fkyaml::node::deserialize(std::string(R"(
mode: emitter
start_size: [0.2, 0.6]
start_speed: {min: 1, max: 2}
start_lifetime: 3
start_rotation: {x: 10, y: 20}
shape: mesh
emit_from: vertices
distribution: even
simulation_space: local
render_mode: stretched
sprite: flame
blend: additive
toon_bands: 4
size_over_life: [{t: 0, value: 0.5}, [1.0, 2.0]]
color_over_life: [{t: 0, color: {r: 1, g: 0, b: 0, a: 1}}, {t: 1, r: 0, g: 0, b: 1, a: 0}]
bursts: [{time: 0.5, count: [3, 5], cycles: 2, interval: 0.1}]
on_death: [{target: sparks, count: 4, inherit_velocity: 0.5}]
velocity: [1, 2, 3]
)"));
    ParticleSettings s;
    parse_particle_settings(node, s);
    expect(s.start_size.min == 0.2f && s.start_size.max == 0.6f, "yaml: [min, max] range");
    expect(s.start_speed.min == 1.0f && s.start_speed.max == 2.0f, "yaml: {min, max} range");
    expect(s.start_lifetime.min == 3.0f && s.start_lifetime.max == 3.0f, "yaml: an integer constant");
    expect(s.start_rotation.min == 10.0f && s.start_rotation.max == 20.0f, "yaml: the editor's {x, y} range");
    expect(s.shape.type == EmitShape::Mesh && s.shape.emit_from == MeshEmitFrom::Vertices &&
               s.shape.distribution == MeshDistribution::Even, "yaml: mesh shape enums");
    expect(s.space == SimulationSpace::Local, "yaml: simulation space");
    expect(s.look.mode == toy::render::ParticleRenderMode::Stretched && s.look.sprite == toy::render::ParticleSprite::Flame,
           "yaml: render mode and sprite");
    expect(s.look.additive == 1.0f && s.look.toon_bands == 4.0f, "yaml: blend: additive, toon bands");
    expect_near(s.size_over_life.evaluate(1.0f), 2.0f, 1e-4f, "yaml: curve keys in both spellings");
    expect_near(s.color_over_life.evaluate(1.0f).b, 1.0f, 1e-4f, "yaml: gradient keys, flat spelling");
    expect(s.bursts.size() == 1u && s.bursts[0].cycles == 2 && s.bursts[0].count.max == 5.0f, "yaml: bursts");
    expect(s.on_death.size() == 1u && s.on_death[0].target == "sparks", "yaml: sub emitters");
    expect(s.velocity == glm::vec3(1, 2, 3), "yaml: a vec3 as a sequence");
    bool threw = false;
    try {
        ParticleSettings bad;
        parse_particle_settings(fkyaml::node::deserialize(std::string("sprite: sparkle\n")), bad);
    } catch (const std::exception&) {
        threw = true;
    }
    expect(threw, "yaml: an unknown enum value is a load error, not a silent default");
}
