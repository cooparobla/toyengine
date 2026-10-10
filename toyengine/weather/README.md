# toyengine/weather

Scene-global weather and time of day. A clock moves the sun and moon across the sky. A catalogue of
weather **conditions** (clear, rain, storm, snow...) blends from one to the next, and together they
drive the sky gradient, ambient light, global fog, the scene's directional light and runtime
effects (rain, snow, mist, dust). Every component can read the result.

Demo scene: `./build/toyengine weather_demo`
([assets/scenes/tests/effects/weather_demo](../../assets/scenes/effects/weather_demo/scene.yaml)).
It is a small hamlet. The clock runs a day in 4 minutes and the random schedule moves through the
stock conditions. Street lamps light up at dusk, and a campfire goes out in the rain and relights
afterwards. Both use `WeatherReactor`, with no code.

| File | Purpose |
|---|---|
| [`weather_profile.h`](weather_profile.h) | The data: `Settings` (the scene's block), `Condition` (a profile), `EffectSpec`, `default_conditions()`, YAML in and out, and `controlled_render_keys()`. |
| [`sky_model.h`](sky_model.h) | Pure math: hour to sun and moon direction, the single directional light (sun by day, moon by night, crossfaded at the horizon), and the day / twilight / night sky palette. |
| [`weather_system.h`](weather_system.h) | `WeatherSystem` (order **40**, runs in edit mode): the clock, schedule, blended transitions, lightning, the atmosphere, sun driving, runtime effects, `WeatherState`, signals, and `toy::weather::current()`. |
| [`ground_probe.h`](ground_probe.h) | `GroundProbe`, the precipitation map: the highest surface under each cell of a grid around the camera (physics raycasts while running, mesh bounds in the editor, water surfaces on top), handed to rain and snow as their `GroundField`. |
| [`weather_surface.h`](weather_surface.h) | `WeatherSurface`: an object (and its children) opting into ground effects. `WeatherDistantLandings`: shows a precipitation system's landings past its own box. |
| [`weather_reactor.h`](weather_reactor.h) | `WeatherReactor`, a component that fades lights, stops / plays particle effects or toggles children by hour, part of day, condition or precipitation. |

Outside this directory:
- `Engine::sync_weather_render_state_()` copies the atmosphere into the live render config
  every frame, and puts the config's own values back when the weather stops.
- [`scene/runtime_object.h`](../scene/runtime_object.h) is the `RuntimeObject` marker for
  objects a system creates.
- The effect object assets live in [`assets/objects/weather/`](../../assets/objects/weather).

## Authoring

A scene keeps its weather under `scene.settings.weather`, next to its render and physics
overrides. It is per scene, not a `config.yaml` section. In the editor, all of it is in
**Properties > World > Weather & Time of Day**.

```yaml
scene:
  settings:
    weather:
      enabled: true
      # Clock
      time_of_day: 9.5          # hours: where the clock starts
      day_length_minutes: 24    # real minutes per game day; 0 = time stands still
      latitude: 35              # tilts the sun's path (0 = overhead at noon)
      north_offset: 0           # turns the sun's path about Z (sunrise toward +X at 0)
      drive_sun: true           # aim / colour the scene's directional light (or make a runtime Sun)
      sun_intensity: 1.2        # noon, clear
      moon_intensity: 0.12
      moon_color: {r: 0.55, g: 0.65, b: 0.9}
      # Sky palette (blended by the sun's height)
      day_zenith: {r: 0.05, g: 0.18, b: 0.55}     # also day_horizon, day_ground,
      twilight_horizon: {r: 0.85, g: 0.42, b: 0.2} # twilight_zenith, night_zenith,
      night_zenith: {r: 0.004, g: 0.007, b: 0.02}  # night_horizon, night_ground
      ambient_day: 1.0          # render ambient_intensity / sky_intensity at noon...
      ambient_night: 0.25       # ...and at midnight
      night_exposure: 0.35      # x render exposure at midnight (so auto exposure keeps night dark)
      # Schedule
      condition: clear          # the start condition
      schedule: random          # fixed | random | cycle
      seed: 0
      ground_height: 0.0        # where mist sits and rain / snow land when nothing else is below
      surface_collision: true   # rain / snow stop on roofs, terrain, water (the precipitation map)
      ground_effects: true      # splashes and spray where rain lands, snow settling (on WeatherSurface objects)
      ground_height_splashes: false # the fallback plane splashes too (scenes without colliders)
      # Lying snow (see "Snow" below)
      initial_snow_cover: 0.0   # 0..1 the scene starts with (a snowy start condition starts at 1)
      snow_accumulate_time: 180 # s of full snowfall below freezing to full cover
      snow_melt_time: 240       # s full cover takes to melt at +5 C (faster warmer / in rain)
      snow_max_depth: 0.3       # m of deep snow (`shader: snow` surfaces) at full cover
      snow_auto_deformers: false # every Rigidbody leaves tracks in deep snow, not only SnowDeformers
      snow_trench_recover_time: 2 # s for a full-depth track to fill back in (0: only while it snows)
      snow_patch_style: soft    # soft (noisy drifts) | hard (round, crisp-edged toon patches)
      snow_patch_size: 1.5      # m, hard patches' typical diameter
      conditions:               # omitted: default_conditions()
        - name: rain
          weight: 1.2           # random schedule: relative chance (0 = only on request)
          duration: [3, 7]      # real minutes before the schedule moves on
          transition: 25        # seconds to blend in
          next: [overcast, storm, cloudy, rain]   # what may follow (empty = anything)
          cloud_cover: 0.9      # greys / darkens the sky
          sun: 0.3              # x the clock's sun
          ambient: 0.75
          sky_tint: {r: 0.85, g: 0.88, b: 0.92}
          fog_density: 0.0127   # global exponential height fog, extinction per metre
          fog_color: {r: 0.5, g: 0.54, b: 0.58}   # the daylight colour; darkened at night
          fog_height_falloff: 40  # metres it thins by e above the base (0 = uniform, fogs the whole sky)
          fog_sky_blend: 0.4
          fog_max_opacity: 1.0
          fog_sun_amount: 0.05
          wind_strength: 5      # m/s
          wind_heading: 30      # degrees it blows toward, CCW from +X
          wind_gust: 0.4
          temperature: 12       # C (gameplay)
          precipitation: 0.6    # 0..1 (gameplay)
          wetness: 0.8          # WeatherState::wetness creeps toward it
          lightning: 0          # flashes per minute
          effects:
            - {prefab: objects/weather_rain, intensity: 1.0, follow: camera, offset: {x: 0, y: 0, z: 4}, wind_influence: 0.6}
```

### The stock conditions

| Condition | Weight | What it does |
|---|---|---|
| `clear` | 3 | Blue sky, a thin haze. |
| `cloudy` | 2 | Greyer, a little dimmer and breezier. |
| `overcast` | 1.5 | Flat grey light, soft sun. |
| `fog` | 0.8 | Dense height fog and a ground mist volume. |
| `rain` | 1.2 | Rain streaks and splashes, wet surfaces. |
| `storm` | 0.5 | Heavy rain, strong gusty wind, lightning, low mist. Only after rain or overcast. |
| `snow` | 0 | Drifting flakes, cold. |
| `blizzard` | 0 | Dense, wind-driven snow, white-out fog. |
| `sandstorm` | 0 | Wind-blown grit, tan haze, a warm sky tint. |

Weight 0 marks "biome" weather that the random schedule never picks on its own. Set it from code, make
it the start condition, or give it a weight.

### Effects

An effect is an **object asset** that is instanced while its condition has weight. Its strength
follows the blend:

- **ParticleSystem.** `rate` is scaled by the intensity. The wind (times `wind_influence`) is
  added to `velocity`. The ground plane is the weather's `ground_height` plus the asset's own.
  Sub-emitter targets (rate 0, such as splashes) are left alone.
- **Volume.** `density` is scaled by the intensity, `color` is darkened by the time of day, and the
  wind sets its drift. On a `ground` anchor, `height_base` is relative to `ground_height`.
- **Anchors.** `camera` sits around the viewer plus `offset`. `ground` sits under the viewer at
  the ground height plus `offset.z`. `world` is fixed at `offset`.
- **Fading out.** An effect that fades to zero is destroyed once its last particles land, so rain
  stops gracefully instead of vanishing.

### Precipitation

Rain and snow are built to stay with the camera and to land on what is actually there:

- **A wrap volume around the camera.** The drops live in a box (`wrap_box`) centred on the
  emitter, which the weather keeps on the camera. A drop leaving one side comes back in on the
  other, so however fast the camera moves or turns the box is already full; nothing has to fall
  from above first. Drops fade toward the box's sides, so the wrap is never seen.
- **The precipitation map** (`ground_probe.h`, with `surface_collision`). A 48 m grid follows the
  camera, holding the highest surface under each 1 m cell: the drops' ground.
  - While the scene runs, cells are found by physics raycasts straight down against solid
    colliders. That is a few hundred rays a frame, new cells first, then a slow refresh. Give
    roofs and awnings colliders.
  - In the editor's edit mode, physics doesn't run, so the map uses mesh bounds. A pitched roof
    counts as flat at its ridge, and very large meshes are left to the plane.
  - A water surface above the bed wins, so rain splashes on a lake rather than its floor.
  - Nothing is born or wrapped in under cover, and nothing falls through a roof into a house.
- **Lit, and clean under TAA.** Drops, splashes and flakes are lit by the sky, the sun and the
  lamps, in their shadows. Rain scatters light forward, so it glints around a street lamp at
  night or against a low sun and goes dark under an awning. Every precipitation system is
  `reactive`, so TAA never smears the fast drops into scratchy streaks.
- **Splashes lie on what they hit.** The map also holds each cell's surface normal. A landing
  passes it on: splash rings and settled flakes (`render_mode: aligned`, `align_to_normal`) lie in
  a sloped roof, and spray kicks off the surface rather than into it.
- **Splashes are per object.** Rain lands on every surface the map finds, so a roof always keeps
  the ground below it dry. But only a surface carrying a `WeatherSurface` component, or sitting
  under a parent that does, makes splash rings, spray or settled snow. Everything else takes the
  drops silently:
  ```yaml
  - type: WeatherSurface      # e.g. on the ground plane or a street
    splashes: true
  ```
  `ground_height_splashes` lets the fallback plane splash too, for scenes without colliders.
- **Splashes far off.** Real drops only exist inside their wrap box (about 18 m). A
  `WeatherDistantLandings` on the drops' object (`radius: 45`, `targets: [weather_rain_splash]`)
  keeps landings coming past it: the weather scatters them over splash-taking surfaces, at the
  same rate per square metre as the drops land, out to the radius. The precipitation map grows to
  cover it. Snow does the same with its settling flakes (35 m).
- **Ground effects** (`ground_effects`). Rain leaves splash rings and spray droplets where it
  lands, on the ground, a roof or a well. Snow leaves flakes that settle and fade. Both are the
  drops' sub emitters, fired only by landings (`on_death_collision_only`), never by drops that
  age out in the air. Off: no splashes, the drops still land.

The stock assets are `objects/weather_rain`, `weather_snow`, `weather_ground_mist` and
`weather_dust` in [`assets/objects/weather/`](../../assets/objects/weather). Any object asset works,
including your own: a wind-blown leaves effect, ash, fireflies.

## What it controls

While the weather is on, it **owns** these render keys (`controlled_render_keys()`) and writes
them every frame: `ambient_intensity`, `sky_intensity`, `sky_zenith/horizon/ground`, `fog_mode`
(set to exponential height fog), `fog_density`, `fog_color`, `fog_sky_blend`, `fog_max_opacity`,
`fog_height_falloff`, `fog_sun_amount`, plus the unused linear-fog distances. The editor
locks these rows ("Set by Weather"). Their own values come back the moment the weather is
switched off.

It **scales** `exposure` toward `night_exposure` at night. This is a multiplier on the
configured value, so the row stays editable. It does not touch `fog_enabled` or the startup switches
`volumetrics_enabled` (mist and dust haze need it) and `transparency_enabled` (particles need it)
are still config.yaml's.

With `drive_sun`, the scene's first directional light is aimed, coloured and dimmed by the clock.
Its authored values come back when the weather lets go. A scene with no directional light gets a
runtime `Sun`.

## Runtime objects

Every object the weather creates hangs under one root, `Weather`, which carries the
`toy::scene::RuntimeObject` marker (`owner_system: "Weather"`). Runtime objects are never written
to the scene file. The editor lists them in the Hierarchy under a locked **Runtime** row, greyed and
with a lock icon, and shows them read-only in Properties. Any system can mark its own spawns with
`toy::scene::spawn_runtime_root()`.

## Reading it from gameplay

```cpp
#include <toyengine/weather/weather_system.h>

// Anywhere, any time (main thread): the active scene's weather. With none, it is noon and clear.
const toy::weather::WeatherState& w = toy::weather::current();
w.hour; w.day; w.phase; w.is_night(); w.daylight; w.sun_direction;
w.condition; w.previous; w.transition;          // "rain", "overcast", 0..1
w.cloud_cover; w.precipitation; w.temperature; w.wetness; w.wind; w.lightning_flash;
w.snow_cover; w.snow_depth;                     // lying snow 0..1, deep-snow metres

// Control and events, through the scene's system.
toy::weather::WeatherSystem* sys = toy::weather::find(scene);   // or engine.weather()
sys->set_condition("storm");          // blends over storm's own transition
sys->set_condition("clear", 5.0f);    // ...or over 5 s (0 = at once)
sys->set_time(21.5f);                 // jump the clock
sys->set_clock_paused(true);
sys->set_snow_cover(1.0f);            // lay snow now (it builds / melts from there)

// How deep the drawn snow is where a character stands (trenches included) -- toyengine/world/snow_system.h.
if (auto* snow = toy::world::find_snow(scene)) float d = snow->depth_at({x, y}, ground_z);
coopa::event::ScopedConnection c1 = sys->on_condition_changed.connect_scoped(
    [](const std::string& from, const std::string& to) { /* music, NPC schedules... */ });
coopa::event::ScopedConnection c2 = sys->on_phase.connect_scoped([](toy::weather::DayPhase p) { /* shops close */ });
// Also on_hour(int), on_new_day(int), on_lightning() (play thunder a moment later).
```

### WeatherReactor

Covers the common cases with no code:

```yaml
- type: WeatherReactor
  hours: [18.5, 6]            # on between these hours (wraps); omit = any hour
  phases: [dusk, night]       # and/or in these parts of the day
  conditions: [rain, storm]   # and/or in these conditions
  min_precipitation: 0.0
  invert: false               # on when the rule is NOT met
  target: [lights, effects]   # lights: fade point / spot lights here and below (a LightFlicker'd
                              #   light through its `dimmer`); effects: stop / play particle
                              #   systems here and below; children: set_active() on its children
  fade: 1.5
```

It runs while the scene simulates (Play, the game), not in the editor's edit mode.

## Transitions

Changing conditions starts a transition from a snapshot of whatever is showing, toward the
target's profile, over the target's `transition` seconds, eased:

- Numbers and colours lerp. Wind lerps as a vector, so it turns rather than dropping through zero.
- Each effect's intensity lerps, so outgoing rain fades while incoming snow builds.
- Interrupting a transition starts the next one from the current blend, with no jump.
- The target's profile is read every frame, so editor edits show at once.

Surface wetness creeps toward the condition's value (about 40 s to soak, 3 min to dry).

## Snow

`WeatherState::snow_cover` (0..1) builds while precipitation falls below +1 C, at
`precipitation / snow_accumulate_time` per second, holds in a dry frost, and melts above +1 C
(`snow_melt_time` at +5 C, faster when warmer or raining). Switching to a snowy condition at
start (or with **Preview**'s instant snap) lays full cover; **Preview > Snow Cover** sets it live.

The renderer reads it through the surface world set (`toyengine/render/surface_world.h`), every
frame from `Engine::sync_surface_state_()`:

- **Cover layer** (`assets/shaders/gfx/surface/snow.glsl`): every opaque material shows snow on
  geometry facing up (steeper faces as the cover deepens), with patchy edges, where the sky is
  open -- the precipitation map's "sky layer", which looks through Rigidbodies, so a passing crate
  does not leave a bare patch. Materials opt out with `snow: false`. `snow_patch_style: hard`
  swaps the soft noise edge for round patches with a crisp, antialiased edge
  (`gfx/surface/snow_patches.glsl`): dots at light cover that grow, merge, and close up at full
  cover; they shrink crisply toward roof edges and steep faces rather than fading.
- **Deep snow** (`shader: snow`, `assets/shaders/snow_surface.glsl`): raised by
  `cover * snow_max_depth * open sky` (times the patch mask under hard patches, so it lies in
  rounded mounds), less the trench field (`toyengine/world/snow_field.h`), which `SnowDeformer`
  objects -- and with `snow_auto_deformers` every Rigidbody -- press into. Tracks settle back over
  `snow_trench_recover_time`, faster while it snows. The field (51.2 m a side) centres on the
  ground point the camera looks at, so zooming out does not drop the tracks under the target, and
  tracks fade out over its outer fifth instead of vanishing at its edge. Gameplay queries mirror
  the shader on the CPU (`SnowSystem::depth_at()`).

The precipitation map (GroundProbe) keeps running while any snow lies, not only while it falls,
reaching at least 32 m around the camera; past it, everything counts as open sky.

## Editor

**Properties > World > Weather & Time of Day:**

- **The header checkbox.** Switching weather on the first time writes out the clock, the palette
  and the stock conditions. The day sky and sun intensity are taken from the scene's current look,
  so switching it on at noon changes little.
- **Preview** (never saved). Shows the live clock and condition. You can:
  - scrub the time;
  - blend to any condition;
  - fast-forward transitions (**Transition Speed** 1x / 4x / 16x / Instant, or **Finish
    Transition** for the running one);
  - run the clock and schedule in edit mode, where they otherwise stand still;
  - **Show Effects**: rain, snow, mist and the rest in edit mode. Off by default, so the
    effects wait for Play; the sky, sun, fog and lying snow preview either way.
- **Clock, Sky, Schedule.** The scene's settings.
- **Conditions.** The list: add, duplicate, remove, reorder, or reset to stock. Below it is the
  selected condition's profile: schedule, sky and light, fog, atmosphere and effects. Renaming a
  condition updates the start condition and every `next` list that names it.

Every edit reaches the running system live, without a scene rebuild. Each edit is one undo step,
and drags merge into one.

## Not here yet

- **The sky, unless it is physical.** With render `sky_model: gradient` (the default) the sky
  is the renderer's three-colour gradient, which the weather colours by the clock. With
  `sky_model: physical` (World tab, Lighting & Sky) the renderer draws a physically based
  atmosphere with a sun disc, a full moon (always opposite the sun: no lunar phases), stars, and
  with `clouds` a raymarched cloud layer; the weather then drives the sun, the moon and
  `cloud_coverage` from the condition's `cloud_cover`, and the gradient colours and the sun's
  colour come from the atmosphere instead of the day / twilight / night palettes. The cloud
  layer is one slab type (cumulus-like); there are no separate cirrus or anvil shapes, clouds
  cast no shadows on the ground, and water reflects the sky's gradient colours rather than the
  clouds.
- **Wet surfaces.** Rain does not darken or wet materials. `WeatherState::wetness` (and the
  surface world UBO's `snow.z`) is there for a surface shader to use.
- **Rain on water.** Rain splashes on a lake's surface, but makes no water ripples (the water's
  ripple rings are few and kept for gameplay wakes).
- **Precipitation in the distance.** Beyond the wrap box (about 18 m) rain and snow are suggested
  only by the fog; there are no distant rain sheets.
- **Thunder.** There is no thunder audio; hook `on_lightning`.
- **Seasons.** There are no seasons or real day-length variation: every day is an equinox day.
