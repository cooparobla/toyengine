---
name: create-particle-effect
description: Create toyengine particle effects -- ParticleSystem components (fire, smoke, sparks, embers, rain, dust, magic, splashes, scattered foliage/rocks via mesh scatter) with emission, shape, motion, colour/size over life, collision, sub-emitters and sprite/mesh rendering, plus LightFlicker for firelight -- usually packaged as an object asset. Use when asked for particles, VFX, an effect, fire/smoke/sparks/rain/snow, a fountain, or scattering meshes over a surface.
---

# Create a particle effect

Parser: `toyengine/particles/particle_yaml.h` (keys, value forms); defaults:
`toyengine/particles/particle_system.h`; module-by-module guide:
**`toyengine/particles/README.md`** (accurate -- read it). Examples:
`assets/objects/props/campfire.yaml` (glow, flames, scattered coals, ember bursts, smoke, flickering
light) and `assets/scenes/tests/effects/particles_test/scene.yaml` (world-space torch trail, mesh scatter on a
mound, orbiting mesh emission, fountain with collision + on-death sub-emitters). Run
`./build/toyengine particles_test`.

**Unlike most components, an unknown enum value THROWS** at scene load -- spell them exactly.

## Value forms

- Range: `1.5`, `[1.0, 2.0]`, `{ min: 1, max: 2 }`
- Curve (over life, t = 0..1): `[{ t: 0, value: 0.2 }, { t: 1, value: 1.0 }]` or a constant
- Gradient: `[{ t: 0, color: { r: 1, g: 0.6, b: 0.2, a: 1 } }, { t: 1, color: {...} }]`

## Keys by module

| Module | Keys |
|---|---|
| Main | `mode` (emitter / scatter / hair), `duration` (5), `looping` (true), `prewarm`, `start_delay`, `play_on_start` (true), `start_lifetime`, `start_speed`, `start_size`, `start_rotation` (deg), `start_color`, `start_color_b` (random blend), `gravity` (x 9.81 toward -Z; negative rises), `simulation_space` (world / local), `simulation` (cpu / gpu; gpu = compute shaders for 100k+ counts, see the particles README "GPU simulation": sub-emitters, scatter, mesh render mode, prewarm and runtime ground fields fall back to cpu with a warning; `particles.gpu_enabled: false` in config forces cpu), `max_particles` (1000; on gpu it is the fixed pool size), `seed`, `time_scale` |
| Emission | `rate` (per s), `rate_over_distance` (moving emitters), `count` (scatter), `bursts: [{ time, count, cycles (0 = forever), interval, probability }]` |
| Shape | `shape` (point / sphere / hemisphere / cone / box / circle / edge / mesh), `radius`, `radius_thickness`, `angle` (cone half-angle), `arc`, `box {x,y,z}`, `length`, `shape_offset`, `random_direction`; mesh: `mesh_path` (default: own mesh), `emit_from` (faces / vertices / edges), `distribution` (random / even / jittered), `normal_offset`, `align_to_normal`, `random_spin`, `inherit_velocity` |
| Motion | `velocity`, `force`, `drag`, `orbital` (rad/s about emitter +Z), `radial`, `tumble`, `angular_velocity`, `noise_strength`, `noise_frequency`, `noise_scroll`, `noise_octaves` |
| Over life | `color_over_life`, `size_over_life`, `alpha_over_life` |
| Collision | `collide`, `ground_height` (a world-Z plane only; world space only), `bounce`, `collision_friction`, `kill_on_collide` |
| Sub-emitters | `on_death: [{ target: <object name>, count, inherit_velocity }]` -- the target system usually has `rate: 0` |
| Render | `render_mode` (billboard / stretched / horizontal / vertical / aligned / mesh / none), `sprite` (soft / circle / puff / flame / spark / ring / star / leaf / texture), `texture` (implies sprite texture), `blend` (alpha / additive) or `additive` 0..1, `lit`, `toon_bands`, `emissive`, `softness`, `soft_distance`, `camera_fade`, `aspect`, `stretch_speed`, `stretch_length`, `opacity`, `pivot`, `flipbook {x, y}`, `flipbook_mode`, `flipbook_fps`, `sort` (distance / none / oldest / youngest), `max_draw_distance`; mesh mode: `render_mesh`, `material` (forced opaque) |

LightFlicker (sibling of a PointLight): `amount`, `speed`, `wobble`, `color_shift`, `seed`.

## Procedure

1. **Layer it.** Real effects are several systems on child objects of one root (campfire: glow,
   flames, coals, embers, smoke, light). Name each child for what it is.
2. Start from the closest example and change numbers; keep `max_particles` near the real peak
   (rate x lifetime) -- it is the memory and cost budget.
3. Fire/sparks/magic: `blend: additive`, `emissive`, small sizes, short lives. Smoke/dust:
   alpha blend, growing `size_over_life`, fading `alpha_over_life`, `lit` for daylight. Moving
   emitters leaving trails: `simulation_space: world` + `rate_over_distance`. Effects that ride
   a spinning object: `simulation_space: local`.
4. Quad particles need `render.transparency_enabled` (on in the shipped config). Mesh-mode and
   scatter particles join the opaque passes.
5. Package as `assets/objects/<effect>.yaml` (create-object) and place it in a scene.

## Verify

Particles need simulated time; compare two moments and a close camera:

```sh
FRAMES=60  .claude/skills/create-scene/scripts/render_scene.sh <scene> "$TMPDIR/fx_a.png"
FRAMES=150 .claude/skills/create-scene/scripts/render_scene.sh <scene> "$TMPDIR/fx_b.png"
./build/toyengine_tests caml_roundtrips_every_asset
./build/toyengine_tests --group particles
```

A scene that fails to load with an enum error points at a misspelt value. Check the effect reads
at the default camera distance, isn't a solid blob (too many / too big), and loops without
popping (`prewarm: true` for ambient effects visible at start).
