# Ambient / global illumination

toyengine has no baked GI probes, no reflection probes, and no IBL — that's a
deliberate omission (see the root [README](../README.md)'s "Explicitly not
included" list). Instead, an analytic sky gradient stands in for indirect
light everywhere: it's both the diffuse irradiance and the specular base for
every shading path. This document is about the one knob that scales it, and
two look-alike knobs that don't do what they appear to.

## TL;DR

Edit `ambient_intensity` in [`assets/config.yaml`](../assets/config.yaml)
(under `render:` → `# --- Lighting ---`), then restart the app — config is
read once at startup, there's no hot-reload:

```yaml
ambient_intensity: 1.0     # scales sky_gradient(N) indirect diffuse
```

It's a plain multiplier. `1.5`–`2.0` is a reasonable first step up from the
default.

## Which dial you actually want

| Knob | Location | Effect |
|---|---|---|
| `ambient_intensity` | `assets/config.yaml:63` | Indirect **diffuse** — the actual ambient light level. Start here. |
| `sky_intensity` | `assets/config.yaml:64` | Indirect **specular** base. Shared with the SSR composite's subtraction, so raising it stays consistent with reflections automatically (see below) — you don't need to touch anything else. |
| `ssgi_intensity` | `assets/config.yaml:140` (default `0.6`) | Screen-space diffuse colour bleed reusing the SSR trace; gated by `ssr_enabled`. A secondary, more local effect — not what "brighter ambient" usually means. |
| `exposure` | `assets/config.yaml:59` | Global brightness multiplier on the *whole* final image, direct light included. Reach for this only if direct light also needs lifting. |

## How the value reaches the GPU

`ambient_intensity` and `sky_intensity` are parsed together into one struct
and threaded through every shading path from that single instance, so one
config edit is guaranteed consistent everywhere — there's no path that could
end up brighter or dimmer than the others by accident:

1. Parsed in `toyengine/core/config.h` into `PixelRenderConfig::indirect`
   (`toyengine/render/pixel_render_config.h`), a shared
   `coopa::gfx::engine::IndirectParams` (gfxcoopa's
   `gfxcoopa/engine/render_features.h`). That struct is also handed directly
   to `SsrPass::Params`, by design — see its doc comment there — so the
   lighting pass and the SSR composite's cancellation math can never disagree.
2. Fanned out to four transports, one per shading path:
   - deferred opaque — push constants (`PixelLightingPushConstants` in
     `toyengine/render/pixel_render_pipeline.h`)
   - forward transparent mesh — UBO field `ForwardGlobals::lighting1.x`
     (`toyengine/render/forward_globals.h`)
   - SDF forward / capture — UBO field `SdfGlobals::lighting1.x`
   - transparent SSR capture — its own push-constant tail

The shading math itself (`assets/shaders/pixel_lighting.frag`, duplicated for
the forward and SDF paths):

```glsl
vec3 ind_diff = sky_gradient(N) * params.ambient_intensity;
vec3 ambient  = (kD_ind * albedo * ind_diff + ind.value) * ao * ssao;
```

`sky_gradient(N)` is a hardcoded analytic function (gfxcoopa's
`assets/shaders/gfx/sky.glsl`) — three colour constants (`SKY_ZENITH`,
`SKY_HORIZON`, `SKY_GROUND`) blended by the surface normal's up-component.
It's the same function that draws the skybox background.

## Gotchas

**Config is startup-only.** There's no config-file watcher in
`toyengine/core/`; each `ambient_intensity` change needs an app restart to
take effect.

**Per-scene directional-light `ambient:` does nothing here.** Scene YAML
(e.g. `assets/scenes/physics_test/scene.yaml`) lets a `DirectionalLight` set
an `ambient: { r, g, b }`. It looks like exactly the right per-scene ambient
knob, and it *is* uploaded — to `LightUBO::dir_ambient`
(`toyengine/render/pixel_render_pipeline.h`) — and every toyengine shader
does declare `dir_ambient` in its light UBO block. But none of them *read*
it. The only shader anywhere that consumes `dir_ambient` is gfxcoopa's legacy
forward `pbr.frag`, which toyengine's render pipeline doesn't use. Setting
this field currently has no visible effect; don't spend time tuning it.

**Ambient hue isn't configurable.** `SKY_ZENITH` / `SKY_HORIZON` /
`SKY_GROUND` are compile-time constants in gfxcoopa's `sky.glsl`, not
uniforms. `ambient_intensity` scales the gradient's brightness only — to
change its colour you'd edit that file directly. Its own comment notes the
constants are kept well below `1.0` on purpose, so the ACES tonemap curve
(which compresses saturation hard near its shoulder) doesn't wash the
gradient out to flat grey — worth remembering if you push `ambient_intensity`
much past `~2.0` and colour starts looking flatter than expected.

## Extending it

Two things this setup can't do today, if a future need comes up:

- **A flat, non-gradient ambient colour term.** Would need a new config
  field plumbed through the same four transports as `ambient_intensity`
  above. Watch the 128-byte push-constant ceiling on the deferred and
  transparent-capture paths — both have `static_assert`s guarding it in
  `pixel_render_pipeline.h`.
- **Real per-scene ambient / GI.** gfxcoopa has unused machinery for this —
  `gfxcoopa/engine/gi/gi_system.h` and an `EnvironmentLight` component — that
  toyengine never wires up. That's the path to per-scene control rather than
  one engine-wide constant.
