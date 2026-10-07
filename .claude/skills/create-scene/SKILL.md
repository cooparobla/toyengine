---
name: create-scene
description: Create or extend a toyengine scene -- assets/scenes/<name>/scene.yaml with objects, components, lights, camera, prefab instances and per-scene render/physics settings -- including test/showcase scenes for a feature (SSR, fog, physics, water...). Use when asked to make, build, lay out, add objects to, or fix a scene or level, or to set up a scene that demonstrates or tests something. Also holds the full component key reference and the headless render script the other create-* skills use to verify their work.
---

# Create a scene

A scene is `assets/scenes/<tags>/<name>/scene.yaml` (tags are folders: test scenes go in
`scenes/tests/<feature>/`, showcases in `scenes/demos/`; see "Tags" in `assets/README.md`).
`./build/toyengine <name>` and `SCENE=<name>` find it by name whatever its tags. It is a tree of named objects, each a list of
components. **The loader is permissive**: unknown component types and unknown keys are
silently skipped, and most enum parsers map an unknown value to the default. A typo renders as
"nothing happened". Take every key from [reference/components.md](reference/components.md),
never from memory.

Read first: `assets/README.md` (naming, shared meshes and materials, scene settings).
Good examples (under `scenes/demos/` and `scenes/tests/<feature>/`): `pixel_demo` (everything
at once), `ssr_test` and `fog_test` (feature benches with a documented header), `physics_test`,
`water_test`, `particles_test`, `terrain_test`. Reference other assets by type and name only
(`materials/steel`, `mesh_path: cube`) -- never through their tag folders.

## Conventions

- **Z-up, metres.** Ground is the XY plane at z = 0. Gravity is -Z.
- Lowercase snake_case for the folder, `scene_name`, and every object name.
- `format: blender` at the top (informational; most scenes use it).
- Transform rotation is Euler **degrees**, applied X then Y then Z. **Always write all three
  axes** of `position`/`rotation`/`scale`: a missing axis reads as 0, so `scale: { x: 2 }` is a
  flat (2, 0, 0) scale.
- Cameras look down their local -Z: `rotation: { x: 90 }` looks along +Y, `x: 70` looks 20
  degrees below horizontal, `z` sets the heading.
- Start the file with a `#` comment block: what the scene is for, how to run it, and a short
  map of what is where. A test scene also lists which `debug_view`s and config keys are worth
  sweeping (see `ssr_test`/`fog_test`).

## Skeleton

```yaml
# <name> -- <one line: what this scene shows or tests>.
#
# Run: ./build/toyengine <name>        (or SCENE=<name>)
format: blender
scene:
  scene_name: <name>
  settings:                      # optional: overrides of config.yaml for this scene only
    render: { fog_density: 0.02 }
    physics: { gravity: { x: 0.0, y: 0.0, z: -9.81 } }
  root_objects:
    - name: ground
      active: true
      components:
        - type: Transform
          position: { x: 0.0, y: 0.0, z: 0.0 }
          rotation: { x: 0.0, y: 0.0, z: 0.0 }
          scale: { x: 10.0, y: 10.0, z: 1.0 }      # plane is 2x2 -> 20 m square
        - type: MeshRenderer
          mesh_path: plane
          material: materials/concrete
        - type: BoxCollider                         # only if things land on it
          size: { x: 2.0, y: 2.0, z: 0.2 }
          center: { x: 0.0, y: 0.0, z: -0.1 }
          material: concrete
      children: []

    - name: crate
      prefab: objects/<asset>                       # an object asset; see create-object
      components:
        - type: Transform
          position: { x: 2.0, y: 0.0, z: 0.5 }
          rotation: { x: 0.0, y: 0.0, z: 0.0 }
          scale: { x: 1.0, y: 1.0, z: 1.0 }

    - name: camera
      active: true
      components:
        - type: Transform
          position: { x: 0.0, y: -12.0, z: 6.0 }
          rotation: { x: 65.0, y: 0.0, z: 0.0 }
          scale: { x: 1.0, y: 1.0, z: 1.0 }
        - type: Camera
          main: true
          projection: Perspective
          fov: 55.0
          near_clip_plane: 0.1
          far_clip_plane: 500.0
        - type: CameraController                    # drag to orbit, scroll to zoom
          mode: orbit
          target: { x: 0.0, y: 0.0, z: 0.5 }
          min_distance: 2.0
          max_distance: 40.0
      children: []

    - name: sun
      active: true
      components:
        - type: Transform
          position: { x: 0.0, y: 0.0, z: 20.0 }
          rotation: { x: 0.0, y: 0.0, z: 0.0 }
          scale: { x: 1.0, y: 1.0, z: 1.0 }
        - type: DirectionalLight
          direction: { x: -0.4, y: 0.35, z: -0.85 }   # the way the light TRAVELS
          color: { r: 1.0, g: 0.96, b: 0.9 }
          intensity: 1.0
          cast_shadows: true
      children: []
```

## Procedure

1. **Decide what the scene must show.** For a feature bench, give each scenario its own spot
   ("station") with identical props where comparison matters, and make sure the default camera
   frames all of them.
2. **Reuse before creating.** Shared meshes (`cube` is centred unit, `plane` is a 2x2 quad,
   `sphere` radius 1...), materials and object assets are listed in `assets/README.md`. New
   meshes/materials/prefabs that only this scene uses go in `assets/scenes/<name>/meshes/`,
   `.../materials/`; paths resolve against the scene folder first, then `assets/`.
3. **Write the YAML** using `type: Foo` entries (not `!Foo` tags; the YAML parser mishandles
   block tags after the first list item). Prefer generating large regular layouts (rows of
   posts, grids, stripes) with a short Python script over hand-typing them; keep the output
   readable (comments per section, one object per block).
4. **Lighting.** One DirectionalLight (only the first active one counts). Ambient light comes
   from config.yaml's `sky_*`/`ambient_intensity`, not from `EnvironmentLight` (that component
   only feeds an unused GI system). Point/spot lights: intensity in the tens to low hundreds
   reads in shade; next to full sun they need high hundreds to over a thousand. Limits: 16 point,
   8 spot, about 4 + 4 with shadows at High.
5. **Per-scene settings.** `settings.render` overrides any `render:` key of `assets/config.yaml`
   at runtime -- except startup-fixed switches (`fog_enabled`, `volumetrics_enabled`,
   `ssr_enabled`, `transparency_enabled`, `aa_mode`, resolutions, shadow-map sizes...), which a
   scene cannot turn on. If the scene needs one, say so in its header comment and tell the user.
6. **Feature components.** Fog volumes, water, terrain, particles, UI and animation have their
   own skills (create-fog-volume, create-water-body, create-terrain, create-particle-effect,
   create-ui, create-animation). Collider `material:` is a bare physics-material name
   (`concrete`), unlike a renderer's `materials/<name>` path.

## Verify (always)

```sh
cmake --build build -j10                       # once, if the build is stale
.claude/skills/create-scene/scripts/render_scene.sh <name>
.claude/skills/create-scene/scripts/render_scene.sh <name> debug_view=albedo save_low_res=true
./build/toyengine_tests caml_roundtrips_every_asset
```

- **Look at the frame** with the Read tool. Check that every station is visible and
  readable from the default camera, nothing is black or blown out, and fog/transparency doesn't
  wash it out. Iterate on values -- the first numbers are rarely right.
- Read the `.log` next to the image: `not found`, `unregistered shader` and parse errors appear
  there. A missing object usually means a misspelt component type or key, not an error.
- The round-trip test walks every YAML under `assets/`; a new scene must pass it.
- Never open a visible window; the script runs headless. Don't run lldb/gdb.
- Mention to the user any startup-fixed config switch the scene needs.
