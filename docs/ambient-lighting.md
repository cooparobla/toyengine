# Ambient / global illumination

toyengine has no baked GI probes, no reflection probes, and no IBL — that's a
deliberate omission (see the root [README](../README.md)'s "Explicitly not
included" list). Instead, an analytic sky gradient stands in for indirect
light everywhere: it's both the diffuse irradiance and the specular base for
every shading path, the visible sky background, SSR's env-specular subtraction,
and (when enabled) fog's sky blend. This document covers the knobs that scale
and colour it, and what "GI" actually means in this engine.

## TL;DR

Edit these in [`assets/config.yaml`](../assets/config.yaml) (under `render:`
→ `# --- Lighting ---`), then restart the app — config is read once at
startup, there's no hot-reload:

```yaml
ambient_intensity: 1.0     # scales the sky gradient's contribution as indirect diffuse
sky_intensity: 1.0         # scales the sky-gradient indirect specular base
sky_zenith:  [0.05, 0.18, 0.55]   # straight up
sky_horizon: [0.25, 0.35, 0.45]   # at the horizon
sky_ground:  [0.05, 0.045, 0.04]  # straight down
```

`ambient_intensity` is a plain brightness multiplier (`1.5`–`2.0` is a
reasonable first step up). The three `sky_*` colours change hue — they're the
actual colours the sky gradient mixes between, and moving them repaints the
ambient light, the skybox, SSR's reflections, and fog's horizon blend all at
once.

## Which dial you actually want

| Knob | Location | Effect |
|---|---|---|
| `ambient_intensity` | `assets/config.yaml` | Indirect **diffuse** brightness — the actual ambient light level. Start here for "brighter". |
| `sky_zenith` / `sky_horizon` / `sky_ground` | `assets/config.yaml` | The sky gradient's actual **colour**. Start here for "different hue". |
| `sky_intensity` | `assets/config.yaml` | Indirect **specular** brightness. Shared with the SSR composite's subtraction, so changing it stays consistent with reflections automatically — you don't need to touch anything else. |
| `ssgi_intensity` | `assets/config.yaml` (default `0.6`) | Screen-space diffuse colour bleed reusing the SSR trace; gated by `ssr_enabled`. A secondary, more local effect — not what "brighter/different ambient" usually means. |
| `exposure` | `assets/config.yaml` | Global brightness multiplier on the *whole* final image, direct light included. Reach for this only if direct light also needs lifting. |

## How the values reach the GPU

All five (`ambient_intensity`, `sky_intensity`, `sky_zenith`, `sky_horizon`,
`sky_ground`) are parsed together into one struct and threaded from that
single instance to every shading path, so one config edit is guaranteed
consistent everywhere — there's no path that could end up a different
brightness or hue than the others by accident:

1. Parsed in `toyengine/core/config.h` into `PixelRenderConfig::indirect`
   (`toyengine/render/pixel_render_config.h`), a shared
   `coopa::gfx::engine::IndirectParams` (gfxcoopa's
   `gfxcoopa/engine/render_features.h`). That struct is also handed directly
   to `SsrPass::Params` and `SkyboxPass::draw()`, by design — see its doc
   comment there — so the lighting pass, the SSR composite, and the skybox
   background can never disagree.
2. Fanned out to several transports, by shading path:
   - deferred opaque (`pixel_lighting.frag`) — `LightUBO`'s trailing
     `sky_zenith`/`sky_horizon`/`sky_ground` fields (gfxcoopa's
     `light_data.h`), alongside push constants for `ambient_intensity`/
     `sky_intensity`
   - forward transparent mesh, SDF forward, SDF capture, transparent/water
     capture — the same `LightUBO` trailing fields (all five bind `LightUBO`
     as set 1 regardless of shading path)
   - skybox background — `SkyboxPass::SkyboxPushConstants`
   - SSR composite — `SsrPass::CompositePushConstants`
   - fog (when `fog_enabled`) — `FogUBO`'s trailing `sky_*` fields

   Every carrier appends its new fields **after** existing ones (or, for
   `LightUBO`, in place of the dead `dir_ambient` slot — see below), so no
   existing struct offset moved and no shader needed anything but an additive
   edit.

The shading math itself (`assets/shaders/pixel_lighting.frag`, duplicated for
the forward and SDF paths):

```glsl
vec3 ind_diff = sky_gradient(N, lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb)
              * params.ambient_intensity;
vec3 ambient  = (kD_ind * albedo * ind_diff + ind.value) * ao * ssao;
```

`sky_gradient()` (gfxcoopa's `assets/shaders/gfx/sky.glsl`) blends three
colours by the surface normal's (or view ray's) up-component. It still has a
zero-argument-colour overload backed by hardcoded `SKY_ZENITH`/`SKY_HORIZON`/
`SKY_GROUND` constants — those are the defaults every new config field above
matches, and the overload is what every shader in gfxcoopa/blendy that never
opted into configurable colour keeps calling unchanged.

## Gotchas

**Config is startup-only.** There's no config-file watcher in
`toyengine/core/`; each change needs an app restart to take effect.

**Directional lights have no `ambient` field.** It used to (a per-light RGB
"fill" colour), but that field was dead everywhere: it fed
`LightUBO::dir_ambient`, which every relevant shader declared and none read —
the only reader anywhere was gfxcoopa's legacy `pbr.frag`, inside a pipeline
(`PbrPipeline`) that no consumer in any repo ever instantiates. It's been
removed from `DirectionalLightComponent` and its scene-YAML parser. The UBO
slot itself survives, renamed to `_reserved_was_dir_ambient`, purely so the
~11 shader `LightUBO` blocks across gfxcoopa/toyengine/blendy that must
byte-match its C++ layout didn't all need editing atomically to shift every
later field's offset. Ambient/indirect light comes from the sky gradient
above, engine-wide — not per directional light.

**Ambient hue is engine-wide, not per-scene.** `sky_zenith`/`sky_horizon`/
`sky_ground` are one set of values for the whole app, read once at startup —
there's no per-scene override. If a specific scene wants a different sky,
today that means shipping a different `config.yaml` for it.

## Is there GI in toyengine?

Screen-space only — no baked or probe-based GI:

- **Sky-gradient indirect diffuse + specular** — described above; this *is*
  the ambient term.
- **SSGI** (`ssgi_intensity`, default `0.6`, gated by `ssr_enabled`) — a
  single-bounce screen-space diffuse colour-bleed: offsets the shading point
  along the surface normal, samples the blurred scene-colour mip at that
  point, adds it as a Lambertian bounce weighted by the SSR trace's hit
  confidence. Denoised for opaque geometry
  (`gfxcoopa/assets/shaders/gfx/ssr_composite_body.glsl`), raw for
  forward/SDF surfaces (`assets/shaders/pixel_forward_shading.glsl`).
- **SSR** — Hi-Z raymarched specular reflections, temporally resolved.
- **SSAO** — multiplies the whole indirect term (diffuse, specular, and the
  SSGI bounce).

What's *not* here: baked irradiance probes, reflection probes, or IBL.
gfxcoopa has a full implementation of these (`gfxcoopa/engine/gi/gi_system.h`
— a CPU-baked spherical-harmonics probe grid plus GGX-prefiltered reflection
probes and a BRDF LUT) and an `EnvironmentLightComponent`, but only blendy
instantiates them; toyengine has zero references to any of it. That's the
path to real off-screen GI if this engine ever needs it — see "Extending it"
below.

## Extending it

- **Per-scene sky/ambient.** Would mean moving `sky_zenith`/`sky_horizon`/
  `sky_ground` (and `ambient_intensity`/`sky_intensity`) off `PixelRenderConfig`
  and onto a scene-level component, uploaded per-scene-load instead of parsed
  once from `config.yaml`.
- **Real GI (baked probes / reflection probes / IBL).** Wire up gfxcoopa's
  `GiSystem` and `EnvironmentLightComponent` (currently blendy-only) — see
  `gfxcoopa/engine/gi/gi_system.h`.
