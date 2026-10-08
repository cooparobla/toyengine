# Scene component reference

Every component a scene or object asset can use, with its exact keys. Colours are
`{ r, g, b }`, vectors `{ x, y, z }`; a partial map keeps the defaults of the missing fields
(except Transform -- write all three axes). **Unknown types, unknown keys, and (unless noted)
unknown enum values are silently ignored or fall back to the default.** ParticleSystem enums
and WaterBody `mode` throw on unknown values instead.

Parsers: `libs/libcoopa/coopa/scene/scene_loader.h` (Transform, objects),
`libs/gfxcoopa/gfxcoopa/engine/components/register.h` (rendering),
`toyengine/scene/register.h` (engine components), `libs/physxcoopa/physxcoopa/physx_yaml.h`
(physics), `toyengine/particles/particle_yaml.h`, `libs/libcoopa/coopa/animation/animation_yaml.h`,
`libs/uicoopa/uicoopa/ui_yaml.h` and `builder/ui_composites_yaml.h`, `libs/sfxcoopa/sfxcoopa/sfx_yaml.h`.
When in doubt, open the parser -- it is short and authoritative.

## Objects

```yaml
- name: thing            # default "Object"
  active: true           # default true
  prefab: objects/x      # optional object asset (= inherit_from:); "path#ObjectName" picks a child
  components: [ ... ]    # `- type: Foo` entries
  children: [ ... ]
```

Prefab instance merge: the instance's root Transform (or RectTransform) **replaces** the
prefab's; other components match by type (add `id:` to tell repeated types apart) and
deep-merge; children match by `name` and merge; unmatched entries are appended; `remove: true`
on a component or child deletes it.

## Core

| Component | Keys (default) |
|---|---|
| Transform | `position`, `rotation` (Euler degrees, X then Y then Z), `scale` |
| Camera | `main` (false; first started wins if none), `projection` (`Perspective` / `Orthographic`), `fov` (60, vertical deg), `orthographic_size` (3, half-height), `near_clip_plane` (0.1), `far_clip_plane` (1000), `lens` (50 mm), `sensor_width` (36), `aperture`, `focus_distance` (0 = config), `focus_object` (`a:b` path) |
| CameraController | `mode` (`orbit` / `fly`), `tracker` (object name to follow), `target`, `target_offset`, `distance`, `yaw_deg`, `pitch_deg` (unset = from the start Transform), `min_pitch_deg` (0), `max_pitch_deg` (85), `min_distance` (0.5), `max_distance` (100), `follow_smoothing` (10), `movement_smoothing` (0), `mouse_sensitivity` (0.15), `zoom_speed` (1), `invert_x`, `invert_y`, `capture_cursor` (true), `auto_rotate_deg_per_sec` (0), `move_speed` (5), `look_speed_deg_per_sec` (90) |

## Rendering

| Component | Keys (default) |
|---|---|
| MeshRenderer | `mesh_path` (key -> `meshes/<key>.yaml`), `material` (path, `{ base: ..., overrides }`, or inline -- see create-material), `materials` (per slot: list or `{ slot_name: material }`), `lod_bias` (1), `lods_enabled` (true), `affects_reflection_probes` |
| SkinnedMeshRenderer | `mesh_path`, `bones` (object paths; default = the mesh's vertex-group names), `rig`. Sibling MeshRenderer has no `mesh_path` (material only) |
| DirectionalLight | `direction` (-1,-1,-1; the way light travels), `color`, `intensity` (1), `cast_shadows` (true), `shadow_intensity` (1). Only the first active one is used |
| PointLight | `color`, `intensity` (1), `range` (10), `cast_shadows` (true), `attenuation_constant` (1; here a falloff SHARPNESS, ~2-8 = crisper edge), `attenuation_linear`, `attenuation_quadratic` |
| SpotLight | PointLight keys + `direction` (0,0,-1, local), `inner_angle` (20), `outer_angle` (30; half-angles, degrees) |
| Volume | see create-fog-volume |
| SdfRenderer | `bounds_center`, `bounds_extent` (local box; keep tight), `material`, `cast_shadows`, `max_steps` (64), `surface_epsilon`, `normal_epsilon`, `smoothing` (blend between child shapes) |
| SdfShape | `shape` (`sphere` / `box` / `plane`), `params` (sphere x = radius, box = half-extents, plane x = offset), `rounding`, `op` (`union` / `subtract` / `intersect`), `blend`. One per object; children of an SdfRenderer |
| EnvironmentLight, ReflectionProbe, GiProbeVolume | parsed but only feed gfxcoopa's GI system, which toyengine does not run. Ambient comes from config `sky_*` |

## Engine behaviours

| Component | Keys (default) |
|---|---|
| KinematicMover | `mode` (`pingpong` / `orbit` / `spin`), `axis` (1,0,0), `distance` (2, peak-to-peak), `speed` (pingpong: Hz; orbit: deg/s), `orbit_center`, `orbit_radius` (2), `spin_axis` (0,0,1), `spin_speed` (90 deg/s) |
| FreeMover | `move_speed` (8), `smoothing` (12) -- WASD-driven object |
| KinematicController | `move_speed` (4), `smoothing` (10), `lock_height` (true) |
| HealthDriver | `bar_object`, `max_health`, `start_health`, `damage_per_second`, `regen_per_second`, `turnaround_fraction` |
| Terrain | see create-terrain |
| WaterBody, Buoyancy | see create-water-body |
| ParticleSystem, LightFlicker | see create-particle-effect |
| WeatherSurface | `splashes` (true). Rain splashes / sprays and snow settles on this object and its children; unmarked surfaces take the drops silently (they still keep the rain off what is below). Usually on the ground plane / streets |
| WeatherDistantLandings | `radius` (45), `targets` (sub emitter names; empty = all). On a precipitation ParticleSystem's object: the weather shows its landings past its wrap box, out to `radius` |
| WeatherReactor | `hours` ([from, to], wraps), `phases` (night / dawn / day / dusk), `conditions` (weather names), `min_precipitation`, `invert`, `target` ([lights, effects] default; also `children`), `fade` (1.5 s). Switches lamps, fires and effects with the time of day / weather -- see toyengine/weather/README.md |
| Animator | see create-animation |
| ClothRenderer | no keys; needs sibling Cloth + MeshRenderer |

## Physics

Shared collider keys: `center`, `is_trigger`, `enabled`, `layer` (int or name from config
`physics.layers`), `material` (bare name -> `physics_materials/<name>.yaml`, or inline map --
see create-physics-material).

| Component | Keys (default) |
|---|---|
| BoxCollider | `size` (1,1,1; FULL size in local units) |
| SphereCollider | `radius` (0.5) |
| CapsuleCollider | `radius` (0.5), `height` (2), `direction` (axis int, 2 = Z) |
| MeshCollider | `mesh_path` (a SEPARATE `<name>_collider.yaml`, never the render mesh's file), `convex` (false = static only) |
| Rigidbody | `mass` (1), `drag` (0), `angular_drag` (0.05), `use_gravity` (true), `is_kinematic` (false), `interpolation` (`Interpolate` / `None`), `freeze_position`/`freeze_rotation` `{x,y,z: bool}`, `velocity`, `angular_velocity` |
| HingeJoint | `connected_object`, `anchor`, `axis` (0,0,1), `limits { min, max }` (degrees) |
| Cloth | `resolution {x,y}`, `size {x,y}`, `mass`, `stretch_compliance`, `bend_compliance`, `damping`, `thickness`, `friction`, `gravity_scale`, `wind`, `self_collision`, `anchors [{ object, point, radius }]` and more -- see physx_yaml.h |

## Audio

| Component | Keys |
|---|---|
| AudioSource | `clip`, `bus` (SFX), `volume`, `pitch`, `loop`, `play_on_start` (false), `spatialize` (mono only), `priority`, `min_distance`, `max_distance`, `curve` (`Inverse` / `Linear` / `Logarithmic`), `rolloff`, `spatial_blend`, `spread`, `doppler`, cone angles |
| AudioListener | `track_velocity` |

There is no `assets/audio/` folder yet; clips resolve against the scene folder, then `assets/`.

## UI

RectTransform, Canvas, Theme, Image, Text, Button, layout groups and the themed composites:
see create-ui.

## Limits

16 point lights, 8 spot lights, 1 directional; ~4 point + 4 spot shadowed at High; 8 Volumes,
4 lights scattering into them; `sdf_max_renderers` 64 / `sdf_max_shapes` 512.
