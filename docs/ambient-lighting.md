# Ambient / global illumination

toyengine has no baked GI probes, no reflection probes, and no IBL. Instead, an analytic sky gradient stands in for indirect
light everywhere: it's both the diffuse irradiance and the specular base for
every shading path, the visible sky background, SSR's env-specular subtraction,
and (when enabled) fog's sky blend. This document covers the knobs that scale
and colour it, and what "GI" actually means in this engine.

## TL;DR

Edit these in [`assets/config.yaml`](../assets/config.yaml) (under `render:`
→ `# --- Lighting & sky ---`), or per scene under `scene.settings.render` (see
[assets/README.md](../assets/README.md)'s "Scene settings"). All five are
runtime tunables: the editor's Render settings apply them live, and a scene's
overrides apply when it becomes active. A running game reads `config.yaml`
once at startup.

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
ambient light, the sky background, SSR's reflections, and fog's horizon blend
all at once.

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

1. Parsed in `toyengine/core/config.h` into `ToyRenderConfig::indirect`
   (`toyengine/render/toy_render_config.h`), a shared
   `coopa::gfx::engine::IndirectParams` (gfxcoopa's
   `gfxcoopa/engine/render_features.h`). The same instance feeds the lighting
   pass's push constants and `SsrPass::Params`, by design — see its doc
   comment there — so the lighting pass, the SSR composite, and the sky
   background can never disagree.
2. Fanned out to several transports, by shading path:
   - deferred opaque and the sky background (`toy_lighting.frag`, which
     draws the procedural sky at background pixels) — `LightUBO`'s
     `sky_zenith`/`sky_horizon`/`sky_ground` fields (gfxcoopa's
     `light_data.h`, mirrored by `assets/shaders/light_ubo_body.glsl`),
     alongside push constants for `ambient_intensity`/`sky_intensity`
   - forward transparent meshes, SDF forward and particles — the same
     `LightUBO` fields
   - SSR composite — `SsrPass::CompositePushConstants`
   - fog (when `fog_enabled`) — the same `LightUBO` `sky_*` fields (its fog block sits beside them)

The shading math itself (`assets/shaders/toy_lighting.frag`, duplicated for
the forward and SDF paths):

```glsl
vec3 ind_diff = sky_gradient(N, lights.sky_zenith.rgb, lights.sky_horizon.rgb, lights.sky_ground.rgb)
              * params.ambient_intensity;
vec3 ambient  = (kD_ind * albedo * ind_diff + ind.value) * ao * ssao;
```

`sky_gradient()` (`gfx/sky.glsl`, one of the shared headers that stay in
gfxcoopa's `assets/shaders/`) blends three colours by the surface normal's
(or view ray's) up-component. It also has a direction-only overload backed by
hardcoded `SKY_ZENITH`/`SKY_HORIZON`/`SKY_GROUND` constants — the same values
as the config defaults above — for shaders (gfxcoopa's, blendy's) that don't
take configurable colours.

## Gotchas

**No config-file watcher.** A running game reads `config.yaml` once at
startup; only the editor (and scene overrides) change these values live.

**Directional lights have no `ambient` field.** Ambient/indirect light comes
from the sky gradient above, engine-wide — not per directional light.

**Ambient hue is per config, optionally per scene.** `sky_zenith`/
`sky_horizon`/`sky_ground` come from `config.yaml`, and a scene can override
them under `scene.settings.render`.

## Is there GI in toyengine?

Screen-space only — no baked or probe-based GI:

- **Sky-gradient indirect diffuse + specular** — described above; this *is*
  the ambient term.
- **SSGI** (`ssgi_intensity`, default `0.6`, gated by `ssr_enabled`) — a
  single-bounce screen-space diffuse colour-bleed. On opaque geometry with
  `ssgi_traced` (the shipped setting) it is one cosine-hemisphere Hi-Z ray per
  pixel (`ssgi.frag`), temporally resolved and denoised in its own chain, at
  the SSR trace resolution divided by `ssgi_resolution_scale` (2 at Low/Medium
  `ssgi_quality`). Without it, and on forward/SDF surfaces
  (`assets/shaders/toy_forward_shading.glsl`), the bounce is a single tap of
  the blurred scene-colour mip at a point offset along the normal.
- **SSR** — Hi-Z raymarched specular reflections with GGX visible-normal ray
  sampling, temporally resolved and roughness-aware denoised. Hits read the
  previous frame's final HDR image, reprojected by the G-buffer velocity, so
  reflections include transparent geometry, fog and other reflections.
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

- **Real GI (baked probes / reflection probes / IBL).** Wire up gfxcoopa's
  `GiSystem` and `EnvironmentLightComponent` (currently blendy-only) — see
  `gfxcoopa/engine/gi/gi_system.h`.
