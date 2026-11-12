# toyengine/env

**Environment** is the umbrella: everything about the world's state at a point in space.
It has two kinds of contributor, and this directory folds them into one answer.

| | Where | Scope |
|---|---|---|
| **The global contributor** | [`toyengine/weather/`](../weather/README.md) | the whole scene: a clock, a weather condition, a mean wind with gusts |
| **Local contributors** | here | a shape in the world: a rotor's downwash, a vent's updraft, a sword swing, a wet patch, a sheltered courtyard |

A consumer never asks which kind it is reading. It asks the field.

Demo scene: `./build/toyengine environment_demo`
([assets/scenes/effects/environment_demo](../../assets/scenes/effects/environment_demo/scene.yaml)).

| File | Purpose |
|---|---|
| [`env_source.h`](env_source.h) | `EnvSource` (one influence: shape, blend and the channels), `EnvPulse` (a one-shot one with an envelope), and the pure samplers `env_falloff()` / `env_wind_full()` / `env_wind()`. |
| [`env_field.h`](env_field.h) | `EnvField` -- the global term plus up to `kMaxEnvSources` locals -- and the samplers everything reads: `wind_at()`, `global_wind_at()`, `local_wind_at()`, `wetness_at()`, `temperature_at()`, `turbulence_at()`, `sample()`. Pure, header-only, trivially copyable. |
| [`env_influence.h`](env_influence.h) | The `EnvInfluence` component (a persistent local influence) and `EnvPulseEmitter` (a one-shot one fired by an animation event). |
| [`env_preset.h`](env_preset.h) | `EnvPresetSwitcher` -- a panel of UI buttons choosing between authored environment presets. |
| [`env_system.h`](env_system.h) | `EnvSystem` (order **45**, runs in edit mode): the gather, the pulse list, `current_field()`, `emit()`, `find_env()`. |

## Why there is so little code here

Every wind reactor in the engine already went through **one** function, duplicated CPU and
GPU:

- `toy::weather::wind_at(wind, gust, t, p)` — [`weather/wind.h`](../weather/wind.h)
- `gfx_world_wind_at(vec3 p)` — [`assets/shaders/gfx/surface/world.glsl`](../../assets/shaders/gfx/surface/world.glsl)

Making those two field-aware is what made foliage sway *and its shadows*, CPU and GPU
particles, cloth, rigidbody drag, fog drift and water all react to local influences. None of
those consumers knows a rotor exists.

**Three things must stay in step**, and there is no compiler or test that will catch it for
you if they drift: `env_field.h`'s fold, `world.glsl`'s `gfx_env_*` fold, and
`surface_world.cpp`'s packing. The comments in all three say so.

## Authoring

```yaml
- type: EnvInfluence
  shape: disc            # sphere | disc (a cylinder about +Z: a rotor column, a vent plume)
  radius: 7              # m; the influence is exactly zero at and beyond it
  height: 10             # disc only: m either side of the origin (0 = an unbounded column)
  falloff: 0.45          # 0 a hard edge .. 1 a soft bowl; full strength inside radius * (1 - falloff)
  mode: radial           # directional | radial (radial aims away from the +Z axis)
  wind: {x: 0, y: 0, z: 0}   # directional: m/s
  outward: 11            # radial: m/s away from the axis, at the rim
  vertical: -16          # radial: m/s along +Z -- NEGATIVE is a downwash
  turbulence: 0.7        # 0..1 extra foliage flutter and particle noise
  wetness: 0.0           # 0..1 added surface wetness (negative = a local dry zone)
  temperature: 0.0       # degrees C offset (gameplay only)
  snow_clear: 1.0        # 0..1 how hard it scours lying snow
  water_disturb: 0.9     # 0..1 ripple rings on water beneath it
  blend: add             # add | max | override
  strength: 1.0          # 0..1 master scale on every channel; 0 costs nothing
  enabled: true
```

In the editor it is **Add Component > Effects > Env Influence**.

### Blends

| | What it does |
|---|---|
| `add` | Summed on top of the weather and every other `add` source: a gust over the prevailing wind. |
| `max` | Only the strongest overlapping `max` source contributes, so two of them do not stack into a hurricane. |
| `override` | Replaces the weather *and* the combined locals inside the shape, eased out by its own falloff. A walled courtyard the wind does not reach is `override` with `wind: 0`. Of several overlapping overrides the one reaching the point most strongly wins, so the result never depends on the order they were gathered in. |

### One-shot pulses

Things with no persistent object — a swing, a blast, a slammed door. `EnvPulseEmitter` wires
one to an animation event, so a sword gusts the ferns with **no game code**: the clip already
carries the event.

```yaml
- type: EnvPulseEmitter
  event: swing           # the Animator clip event that fires it
  offset: {x: 0, y: 0, z: 1.2}   # where the pulse is born, in the object's own space
  radius: 2.8
  mode: radial
  outward: 10
  turbulence: 0.9
  attack: 0.04           # s to full
  hold: 0.05             # s at full
  decay: 0.3             # s back to nothing -- the ferns snap, then recover
```

From code, with no component at all:

```cpp
#include <toyengine/env/env_system.h>

toy::env::EnvPulse p;
p.source.position = blade_tip;
p.source.radius   = 2.5f;
p.source.mode     = toy::env::EnvMode::Radial;
p.source.outward  = 12.0f;
p.decay           = 0.4f;
toy::env::emit(scene, p);     // fire and forget; EnvSystem ages and drops it
```

## What reacts, and how

Everything in [`weather/README.md`](../weather/README.md)'s "Wind" table, because they all
read the field now. What a local source adds on top:

| What | Local behaviour |
|---|---|
| Foliage (`shader: foliage`) | Leans with the local wind at the plant's root, like the weather's. **A downward local wind also presses the plant flat** — the weather's wind is always horizontal, so that term is exactly zero without a local source. `turbulence` speeds the flutter. |
| Particles | Carried by `wind_at()` per particle (CPU) or at the emitter (GPU), through `wind_influence` as before. |
| Cloth, rigidbodies | `weather_wind` and `wind_influence` now sample the full field, so they respond with no weather in the scene at all. |
| Fog volumes | `wind_influence` picks up the local wind. Deliberately NOT the weather's gusts — that would make every existing volume's drift wobble. |
| Water | `water_disturb` emits expanding rings (`WaterSystem::emit_ripple()`) beneath the source. The per-body wave response stays on the weather's mean wind: it rewrites the body's material when it changes, which is far too expensive to drive from something that moves every frame. |
| Lying snow | `snow_clear` stamps the same trench field a `SnowDeformer` presses into, so a scoured pad drifts back in over `snow_trench_recover_time` for free. |
| Surface wetness | `wetness` shows wherever `gfx_env_wetness_at()` is read — today the heightmap terrain surface. |

## Switching presets from a UI

`EnvPresetSwitcher` (env_preset.h) turns a panel of buttons into a choice of environments. A
preset is a subtree of the scene plus, optionally, a weather condition: picking one activates
its subtree, deactivates every other preset's, and blends the weather.

```yaml
- type: EnvPresetSwitcher
  start: calm                  # which preset is live on load
  label_object: PresetLabel    # optional Text widget shown the active blurb
  presets:
    - {name: calm,   button: BtnCalm,  object: preset_none,  condition: still, blurb: "Still air."}
    - {name: rotor,  button: BtnRotor, object: preset_rotor, condition: still, blurb: "A downwash."}
    - {name: gale,   button: BtnGale,  object: preset_none,  condition: gale,  blurb: "The global wind."}
```

Buttons are addressed by the NAME of their UI object, on the scene event bus -- the same
channel `SignalReactor` listens on -- so the panel can be its own UI asset anywhere in the
scene and knows nothing about environments.

One component rather than a `SetActiveOnSignal` per button, because presets are **exclusive**:
"turn this one on and the others off" needs an instance per (button, object) pair otherwise,
and nothing would keep the weather in step.

Deactivating is what switches an influence off -- the gather skips an `EnvInfluence` whose
object, or any ancestor, is inactive.

## Reading it from gameplay

```cpp
#include <toyengine/env/env_system.h>

const toy::env::EnvField& f = toy::env::current_field();   // any component, any time
glm::vec3 w = toy::env::wind_at(f, position);              // weather + gusts + locals
float wet   = toy::env::wetness_at(f, position);
float degc  = toy::env::temperature_at(f, position);

toy::env::EnvSample s = toy::env::sample(f, position);     // all of it at once
```

`toy::weather::current().wind_at(p)` still works and is still the *weather-only* wind. The
env field is the full answer.

## The position contract

A source's position comes from its object's world matrix, which **TransformResolve finalises
at order 350 — after this gather at 45.** A source on a physics-driven or animated object is
therefore reading last frame's resolved transform, extrapolated one frame from the frame
before it (which cancels the lag for anything moving at a steady speed).

This is deliberate. One snapshot per frame is the property that makes the field testable and
keeps every consumer consistent: a second gather after TransformResolve would leave the grass
and the GPU on frame N while cloth, water and rigidbody drag were still on frame N-1, which
reads as a wake that leads its helicopter and is far harder to debug than a third of a metre
of lag.

Contrast `SnowDeformer` (order 370), which stamps where it is drawn — it runs after
TransformResolve and has no CPU consumers downstream of it.

## Limits

- **`kMaxEnvSources` is 8** (`env_field.h`). Not a UBO budget — the GPU fold runs per vertex
  in the foliage G-buffer stages *and* in their shadow and shadow-cube twins, over tens of
  thousands of grass instances. The gather keeps the strongest by speed x radius and prints
  what it dropped; a silent cap would read as "it covered everything".
- Every sampler early-outs on an empty source list, so a scene that authors none pays nothing.
- **Temperature never reaches the GPU.** Nothing draws from it; it is there for gameplay and
  for local snow melt. `snow_clear` is what makes a warm spot visible.
- A pulse emitted from gameplay (the Behaviour walk, order 200, or an animation event at 300)
  is folded in by the *next* frame's gather. The attack envelope absorbs that.

## Not here yet

- **No local weather condition.** A rain shower over one valley is not expressible: the
  condition, the sky and the global fog are scene-wide.
- **No local snow *cover*.** `snow_cover` is one global scalar; `snow_clear` scours the trench
  field instead, which is what the shader already draws.
- **No wet-surface shading on stock materials.** `gfx_env_wetness_at()` is read by the
  heightmap terrain surface only; a plain `MeshRenderer` does not darken when it is wet.
- **No audio.** Nothing plays a rotor's wind; hook `EnvPulseEmitter::on_fired` or
  `EnvSystem::on_pulse`.
- **No box or cone shapes.** Sphere and disc cover a rotor, a vent and a swing. A box would
  cost an inverse matrix per source in four shader stages.
