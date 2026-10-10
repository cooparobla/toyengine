---
name: create-fog-volume
description: Add fog and volumetric lighting to a toyengine scene -- local Volume components (uniform fog pockets, drifting haze, wind ribbons; box or sphere) lit by the sun with shadows (god rays) and by point/spot lights (visible light cones, lantern halos) -- and tune the global analytic fog via scene settings. Use when asked for fog, mist, haze, smoke banks, god rays / light shafts, light cones, atmosphere or volumetrics in a scene.
---

# Add fog and volumetrics

Two separate systems:

| System | What | Switch (assets/config.yaml, startup-fixed) |
|---|---|---|
| Local volumes | `Volume` components: bounded, marched, shadowed by the sun, lit by up to 4 point/spot lights | `volumetrics_enabled` (on) |
| Global fog | analytic distance + height fog over everything, `fog_*` keys | `fog_enabled` (on by default) |

A scene **cannot** turn either switch on (startup-fixed); it can tune their runtime keys in
`scene.settings.render`. Parser for Volume: `libs/gfxcoopa/gfxcoopa/engine/components/register.h`
("Volume"); struct docs: `libs/gfxcoopa/gfxcoopa/engine/components/volume.h`. Worked example of
every use: **`assets/scenes/tests/rendering/fog_test/scene.yaml`** (header explains each station).

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
      fog_mode: 1                      # 1 exponential height fog (0 = legacy linear ramp)
      fog_density: 0.015               # extinction per metre at/below the base
      fog_color: [0.62, 0.66, 0.74]
      fog_height_base: 0.0             # full density at/below this Z (constant below it)
      fog_height_falloff: 6.0          # metres over which it thins by e above the base; <= 0 = uniform
      fog_sun_amount: 0.5              # directional sun in-scatter (HG lobe)
      fog_start_distance: 0.0          # no fog nearer than this
      fog_cutoff_distance: 0.0         # stop accumulating past this distance (0 = none)
```

The global fog is Unreal-style exponential height fog: transmittance is exp(-optical depth), with
the depth integrated in closed form along each view ray. It is applied per medium -- the opaque
scene and the sky by the fog pass, and every translucent surface (water, glass, particles, SDF
glass) by its own shader at its own distance. Sky pixels integrate to `fog_sky_distance`
(1000 m): with a height falloff the horizon fogs and the zenith stays clear, and with uniform fog
(`fog_height_falloff: 0`) the whole sky turns fog-coloured, so keep a falloff unless that is the
look you want. With the camera under water, only the part of a ray above the surface is fogged.
`fog_mode: 2` (Exp2) and `fog_max_distance` are retired: they load as exponential and are ignored, respectively.

Only takes effect while `fog_enabled: true` in config.yaml (the default) -- tell the user if the scene relies
on it.

`fog_sky_blend` blends the fog toward the sky's gradient colours. Under the physical sky
(`sky_model: physical`) those colours are computed from the atmosphere every frame, so fog picks
up sunset and night tints by itself; the physical sky's haze is its own key,
`atmosphere_density` (1 clear .. 4+ hazy). A cloud layer (either sky model) is `clouds: true`
with `cloud_coverage` / `cloud_altitude` / `cloud_thickness` / `cloud_density` / `cloud_wind_speed`
(the weather drives `cloud_coverage` while it is on); with `cloud_shadows: true` the clouds also
shadow the volumetric fog's sunlight, so light shafts fall through the gaps between them.

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
