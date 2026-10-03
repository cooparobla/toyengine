# toyengine/water

Bodies of water and the things that float on them: planar lakes and oceans, flowing rivers, and
pontoon buoyancy in the style of Unreal's `BuoyancyComponent` and the usual Unity floater setups.

Demo scenes:
- `cplay water_test` ([assets/scenes/water_test](../../assets/scenes/water_test)): a lake with a
  beach, a steep bank and an island; a river running down a hill through rapids into it; rocks, a
  pier and a river boulder; crates, a barrel, a ball, a raft and a stone in the water; a kinematic
  boat circling and leaving a wake.
- `cplay underwater_test` ([assets/scenes/underwater_test](../../assets/scenes/underwater_test)):
  the camera starts under the surface of a deep pool. Zoom out or lower the pitch to rise through
  the waterline.

| File | Purpose |
|---|---|
| [`water_waves.h`](water_waves.h) | Gerstner wave sum, its depth attenuation, and the height inversion CPU queries need. **The C++ mirror of [`assets/shaders/water_waves.glsl`](../../assets/shaders/water_waves.glsl)** — the one place the surface shape is defined; keep the two in sync. |
| [`water_surface_query.h`](water_surface_query.h) | `WaterSurfaceQuery` — a body's baked, undisplaced, world-space triangles in a uniform XY grid. `sample(p) -> {surface height, normal, current, depth, turbulence}`, waves included. |
| [`water_flow_bake.h`](water_flow_bake.h) | `bake_flow()` — derives a flowing body's per-vertex current and turbulence from its mesh: downhill along the slope, faster where steeper, UV `u` where level, bent around static obstacles. Pure CPU, unit-tested with no physics. |
| [`water_body.h`](water_body.h) | `WaterBody` — the scene component: mode (`planar` / `flowing`), geometry source, waves, flow, look, density. |
| [`buoyancy.h`](buoyancy.h) | `Buoyancy` — the scene component that makes a sibling `Rigidbody` float: pontoons, volume, drags. |
| [`water_system.h`](water_system.h) | `WaterSystem` + `install_water_system()`, order **90**: bakes every body (GPU mesh + CPU query), runs buoyancy inside the physics substep, emits ripple rings from moving bodies, and answers `underwater_at()`. |

Rendering pieces outside this directory: [`assets/shaders/water_surface.glsl`](../../assets/shaders/water_surface.glsl)
(the surface, both sides), [`render/passes/underwater_pass.h`](../render/passes/underwater_pass.h) +
[`assets/shaders/underwater.frag`](../../assets/shaders/underwater.frag) (the underwater look), and
`Engine::sync_water_render_state_()`, which hands `render::WaterFrameState` (ripples + underwater
parameters) from this module to the renderer each frame.

## Authoring

```yaml
# A lake: a procedural grid, or `mesh_path:` for any shape.
- type: MeshRenderer            # material only -- no mesh_path; WaterSystem publishes the mesh
  material: { albedo: {r: 0.03, g: 0.16, b: 0.22}, alpha: 0.4, alpha_mode: BLEND,
              refraction: true, ior: 1.33, refraction_tint: {r: 0.45, g: 0.78, b: 0.82} }
- type: WaterBody
  mode: planar
  size: { x: 46.0, y: 46.0 }
  resolution: 96
  wave_amplitude: 0.12          # base wave; three shorter ones are derived from it
  wave_length: 6.0
  wave_direction: 30.0          # degrees

# A river: any mesh whose surface descends; UV u running downstream helps on level reaches.
- type: WaterBody
  mode: flowing
  mesh_path: river_water

# Something that floats: floating height follows mass / (1000 kg/m^3 * collider volume).
- type: Rigidbody
  mass: 300.0
- type: Buoyancy                # pontoons generated from the colliders unless listed
```

The material keeps its usual meaning: `albedo` is the deep-water colour, `alpha` the opacity of
shallow water, `refraction_tint` the Beer-Lambert absorption. `clarity` (on the `WaterBody`) is
the depth at which water reads ~63% opaque.

## How it fits together

```
WaterSystem::execute()                 (order 90, just before Physics at 100)
├─ bake any body that is new, moved, or still missing its physics-aware stage
│    weld -> world space -> bake_flow() (flowing) -> depth probes (raycasts)
│    -> WaterSurfaceQuery::build() -> Mesh::from_arrays() -> sibling MeshRenderer
└─ refresh the buoyant-body list
PhysicsSystem::execute()               (order 100)
└─ PhysicsWorld::step() -> per substep: on_substep -> WaterSystem::substep_()
                                          per body, per pontoon: sample, buoyancy, drag
Renderer: transparent pass, "water" derived shader (water.vert / water.frag / water_capture.frag)
```

### The baked vertex

`WaterSystem` owns each body's GPU mesh, and packs what the shader needs into the standard vertex:
`uv = (water depth below the vertex, turbulence)` and `tangent = (object-space current, 2.0)`.
`tangent.w == 2` marks a baked vertex; any other mesh drawn with `shader: water` is read as
still, deep water. Waves and look parameters go in `PBRMaterial::shader_params` (the base wave —
it reaches both the forward and the reflection-capture vertex stage) and `shader_params_ext`
(foam colour/amount, shore-foam depth, edge-fade depth, ripple strength/scale — forward fragment
stage only).

### The shader

- **Vertex:** the Gerstner sum, attenuated by the baked depth so waves calm over shallows, tilting
  the mesh's own normal (a river surface slopes). Hands `(current.xy, turbulence, crest)` to the
  fragment stage through the backbone's opt-in `GFX_SURFACE_CUSTOM_VARYING`.
- **Fragment:** measures the water under each pixel from the opaque depth buffer. That one number
  drives the shallow-to-deep colour (Beer-Lambert over the real depth, opacity rising toward the
  deep colour), the soft contact edge, and the broken foam band along shores and around anything
  that pierces the surface, static or floating, with no per-object setup. Ripple normals and
  white water are advected along the current with two-phase flow mapping (Vlachos, *Water Flow
  in Portal 2*), so a river visibly runs downhill and churns through rapids and behind obstacles.

### Ripples

Every frame, each Rigidbody crossing a surface (dynamic or kinematic, with or without `Buoyancy`)
may emit an expanding ring:
- a **splash** when it enters the water moving down fast;
- a **wake** trail while it moves relative to the current, one ring per spacing travelled (a crate
  drifting with a river leaves none);
- a **bob** ring now and then while it heaves in place.

Strength scales with speed and size, and a body at rest emits nothing. Rings live 3 s, capped at
the newest 64 (`WaterSystem::emit_ripple()` adds one by hand). The water shader draws each ring as
an outward wave packet that widens and decays, with a foam crest while it is young and strong.
Ripples are visual only: buoyancy does not feel them.

### Underwater

`WaterSystem::underwater_at(camera)` picks the water body above the camera. Each `WaterBody`
carries its own underwater look: `underwater_color`, `underwater_visibility`,
`underwater_absorption` and `caustics`. `UnderwaterPass` runs right after the transparent pass:
- **Which pixels:** only those whose view ray starts below the surface. A camera straddling the
  waterline gets a split image.
- **Fog and absorption:** applied over the ray's in-water length only, up to the geometry or to
  where it leaves through the surface. Red is absorbed first, and the in-scatter is brighter
  looking up than looking down.
- **Caustics:** on submerged, upward-facing surfaces, fading with depth.
- **Shimmer:** a slight screen distortion.

The surface itself is two-sided. From below, light leaving the water refracts with the inverse
IOR, so Snell's window shows the world above. Outside the window (beyond ~48.6°), total internal
reflection turns the underside into a dark mirror.

### Buoyancy

Each body's displaced volume is split across pontoons (a box becomes a 2×2×2 lattice, a sphere
one point, a capsule three; or list them). Per substep and pontoon, the system samples the
surface and applies, at the pontoon, an Archimedes impulse `rho g V f h` plus vertical drag. Off-centre
application gives righting torque and wave rocking. Horizontal drag acts through the centre of
mass, on velocity relative to the mean current under the body, which is what carries floaters
downstream. Linear drag plus quadratic form drag (`0.5 rho Cd A |v|`) are both applied as a
clamped fraction of the relative velocity, so they are unconditionally stable.

## Things worth knowing before changing anything here

1. **CPU and GPU waves must agree.** Buoyancy samples `water_waves.h`; the screen shows
   `water_waves.glsl`. Change one, change the other — `water_wave_height_inverse_matches_forward`
   only checks the C++ side against itself.
2. **Impulses, not forces.** `on_substep` fires *after* `integrate_forces()` (see physxcoopa's
   `world.h`), so a force added there lands a substep late. Buoyancy uses
   `Body::apply_impulse_at_position(..., wake_body = false)`, so a crate on calm water can still
   fall asleep. A sleeping body is only woken when the water under it moves (waves or current).
3. **The submersion ramp is per pontoon and orientation-independent.** Making it depend on the
   body's rotation looks more accurate, but it makes the field non-conservative, and that pumps
   energy into a slow rolling drift that never damps.
4. **Horizontal drag goes through the centre of mass.** Applied per pontoon, it lands almost
   entirely below the centre of mass of a floating body and couples sway into roll and yaw.
5. **Depth and obstacles need physics.** The first bake runs before `PhysicsSystem` has gathered
   colliders (it runs after this system), so it has no depth or obstacle data. The bake reruns
   once physics has executed (`bake_stage == 2`). Obstacles are static colliders with near-vertical
   faces; a gently shelving bank is not one.
6. **Rivers are height fields.** Queries project onto XY, which is fine for lakes, oceans and
   rivers but not for a waterfall's vertical face. A waterfall is a steep river section.

## Not here yet

- Splash particles and spray; god rays under water.
- Ripples that displace the surface geometry or push floating bodies. They only bend the shading
  normal and add foam.
- Caustics seen from ABOVE the water, on a lake bed viewed through the surface. They are only
  drawn while the camera is under water.
- Water surfaces for mapcoopa's rivers in the streamed terrain (`toyengine/world/`). The pieces
  are here: a `WaterBody` with `set_geometry()` along `MapRiver::points`.
