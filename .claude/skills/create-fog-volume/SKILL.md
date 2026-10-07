---
name: create-fog-volume
description: Add fog and volumetric lighting to a toyengine scene -- local Volume components (uniform fog pockets, drifting haze, wind ribbons; box or sphere) lit by the sun with shadows (god rays) and by point/spot lights (visible light cones, lantern halos) -- and tune the global analytic fog via scene settings. Use when asked for fog, mist, haze, smoke banks, god rays / light shafts, light cones, atmosphere or volumetrics in a scene.
---

# Add fog and volumetrics

Two separate systems:

| System | What | Switch (assets/config.yaml, startup-fixed) |
|---|---|---|
| Local volumes | `Volume` components: bounded, marched, shadowed by the sun, lit by up to 4 point/spot lights | `volumetrics_enabled` (on) |
| Global fog | analytic distance + height fog over everything, `fog_*` keys | `fog_enabled` (OFF by default) |

A scene **cannot** turn either switch on (startup-fixed); it can tune their runtime keys in
`scene.settings.render`. Parser for Volume: `libs/gfxcoopa/gfxcoopa/engine/components/register.h`
("Volume"); struct docs: `libs/gfxcoopa/gfxcoopa/engine/components/volume.h`. Worked example of
every use: **`assets/scenes/fog_test/scene.yaml`** (header explains each station).

## Volume

```yaml
- name: ground_mist
  components:
    - type: Transform                    # centre of the volume (rotation/scale apply)
      position: { x: 0.0, y: 0.0, z: 2.0 }
      rotation: { x: 0.0, y: 0.0, z: 0.0 }
      scale: { x: 1.0, y: 1.0, z: 1.0 }
    - type: Volume
      kind: fog                          # READ FIRST: sets kind-specific defaults
      shape: box                         # box | sphere (extent.x = radius)
      extent: { x: 20.0, y: 20.0, z: 2.0 }   # HALF-extents
      density: 0.015
      falloff: 0.05                      # edge softness 0..1 (0 = hard)
      height_base: 0.0                   # full density at/below this world Z...
      height_falloff: 0.7                # ...decaying exponentially above it (<= 0 = off)
      occlusion: 0.5                     # how much it veils what's behind (0 = pure glow)
      sun_amount: 0.7                    # weight of sun + point/spot in-scatter
      color: { r: 0.78, g: 0.82, b: 0.88 }
```

| kind | Use | Defaults applied |
|---|---|---|
| `fog` | uniform pocket, ground mist, a lit room's air | static, density 0.5, occlusion 0.8, sun_amount 0.2, no height falloff |
| `haze` | large soft drifting billows (fbm noise) | speed 0.6, density 0.1, noise_scale 0.05, coverage 0, height_falloff 12, occlusion 0.6 |
| `wind` (default) | thin advected ribbons | density 0.3, coverage 0.86, streak 8, sharpness 3, occlusion 0.15 |

Noise/motion keys: `direction`, `speed`, `noise_scale`, `streak`, `coverage`, `sharpness`
(wind), `gate_scale`, `flow_warp`, `flow_scale`, `octaves`, `detail_gain`.

## The rules that decide whether it looks right

- **`color` is a flat glow added wherever there is density, lit or not; `sun_amount` scales
  sun AND point/spot in-scatter (shadowed).** A volume meant to SHOW light (spotlight cones,
  lantern halo, shafts) wants a dark `color` (~0.03-0.2) and a high `sun_amount` (1.5-3). A
  volume meant to read as mist by itself wants the opposite.
- **Density is per metre and the camera looks through tens of metres.** Scene-wide mist is
  ~0.01-0.03; local pockets 0.3-1.2. 0.1 over a 40 m view is a white-out.
- **Light cones need shade.** Spot/point in-scatter only reads where the sun isn't lighting the
  same volume: roof it, put it in the sun's shadow, or keep the volume small. Local lights next
  to full sun need intensities in the high hundreds+.
- **Use `fog`, not `haze`, for small lit volumes**: haze noise leaves thin patches at a few-metre
  scale that can hide a whole light cone.
- **God rays**: a volume, sun shadows on (`volumetrics_shadows_enabled`), an occluder with gaps
  (slats, columns, trees) between the sun and the volume, and a camera looking toward the sun
  (forward scattering, `volumetrics_sun_anisotropy`).
- Bright wisps (wind) only read against something darker than themselves.
- Limits: 8 volumes per scene, 4 lights scattering (nearest the camera).

## Global fog (scene settings)

```yaml
scene:
  settings:
    render:
      volumetrics_max_distance: 70.0   # march as deep as the scene
      fog_mode: 2                      # 0 linear, 1 exp, 2 exp2
      fog_density: 0.018
      fog_color: [0.62, 0.66, 0.74]
      fog_height_base: 0.0
      fog_height_falloff: 6.0
      fog_sun_amount: 0.5
      fog_max_distance: 90.0
```

Only takes effect when `fog_enabled: true` in config.yaml -- tell the user if the scene relies
on it.

## Verify

```sh
.claude/skills/create-scene/scripts/render_scene.sh <scene>
.claude/skills/create-scene/scripts/render_scene.sh <scene> debug_view=volumetrics
.claude/skills/create-scene/scripts/render_scene.sh <scene> volumetrics_mode=raymarch   # cross-check
.claude/skills/create-scene/scripts/render_scene.sh <scene> fog_enabled=true
./build/toyengine_tests caml_roundtrips_every_asset
```

Look at each frame and iterate: the first densities are almost always too high. If a light
effect is missing, check the lit surfaces with `debug_view=direct` to confirm the light itself
reaches there before blaming the volume.
