---
name: create-water-body
description: Add water to a toyengine scene -- a WaterBody (lake/pond with waves, or flowing river) with its translucent refractive surface material, shore foam, ripples and underwater look, plus floating objects with Buoyancy. Use when asked for water, a lake, pond, pool, river, ocean, waves, or things that float.
---

# Add water

Read **`toyengine/water/README.md`** (system overview, tiers, gotchas) and copy from
`assets/scenes/tests/water/water_test/scene.yaml` (lake + river + floating props) or
`assets/scenes/tests/water/underwater_test`. Parsers: `toyengine/scene/register.h` (WaterBody, Buoyancy);
defaults in `toyengine/water/water_body.h`, `buoyancy.h`.

## Surface

A WaterBody bakes and publishes its mesh to a **sibling MeshRenderer that has no `mesh_path`**
(material only):

```yaml
- name: pond
  components:
    - type: Transform
      position: { x: 0.0, y: 0.0, z: -0.3 }     # the water level
      rotation: { x: 0.0, y: 0.0, z: 0.0 }
      scale: { x: 1.0, y: 1.0, z: 1.0 }
    - type: MeshRenderer
      material: materials/water                 # or inline: BLEND, alpha ~0.4, refraction, ior 1.33
    - type: WaterBody
      mode: lake                                # lake | planar | static | river | flowing (anything else THROWS)
      size: { x: 12.0, y: 8.0 }
      resolution: 48
      wave_amplitude: 0.05                      # 0 = calm
      wave_length: 4.0
      wave_direction: 30.0                      # degrees CCW from +X
      foam_amount: 1.0
      shore_foam_depth: 0.6
      clarity: 3.0
```

Keys: geometry `mode`, `mesh_path` (river: a CPU source mesh -- a separate file from any render
mesh), `size`, `resolution`, `tile_size`; waves `wave_amplitude`, `wave_length`,
`wave_steepness`, `wave_direction`; river `flow_speed`, `flow_min_speed`, `flow_slope_gain`,
`obstacle_radius`, `wake_length`; look `foam_color`, `foam_amount`, `shore_foam_depth`,
`edge_fade_depth`, `ripple_strength`, `ripple_scale`, `clarity`; underwater `underwater_color`,
`underwater_visibility`, `underwater_absorption`, `caustics`; physics `density`, `max_depth`.

The global fog (render `fog_*`) and water cooperate: the water surface is fogged at its own
distance, so it hazes exactly like the shore beside it, and with the camera under water the air
fog applies only beyond where a view ray leaves the surface. The underwater look is the
`underwater_*` keys alone; there is no need to turn the global fog down for underwater shots.

The WaterBody itself sets the material's `shader: water`, its `shader_params` (waves) and
`shader_params_ext` (foam, ripples) from the WaterBody keys -- don't set those on the material;
tune the WaterBody instead. The material must be **BLEND** (`materials/water` is the shared
one); it needs
`render.transparency_enabled` and `refraction_enabled` (both on in the shipped config) and
`water_quality` sets the detail tier. Shore foam and depth colour come from the opaque depth
under the water, so give it a floor/basin (a mesh, with a separate `_collider` mesh if things
should hit it).

## Floating objects

```yaml
- type: Rigidbody
  mass: 40.0
- type: BoxCollider
  size: { x: 1.0, y: 1.0, z: 0.5 }
  material: wood
- type: Buoyancy
  subdivisions: 2           # 1-4 sample density
  linear_drag: 1.5
  angular_drag: 1.0
  # volume: -1 (from colliders); pontoons: [{ x, y, z, radius }] for explicit floats
```

Float height follows mass / (1000 x volume): too heavy sinks, too light bobs high.

## Verify

```sh
.claude/skills/create-scene/scripts/render_scene.sh <scene>
FRAMES=300 .claude/skills/create-scene/scripts/render_scene.sh <scene> "$TMPDIR/water_late.png"   # floaters settled
./build/toyengine_tests caml_roundtrips_every_asset
./build/toyengine_tests --group water
```

Look for: water visible and refractive, foam at the shoreline, floaters at a sensible depth
rather than sunk or flying.
