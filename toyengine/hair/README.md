# toyengine/hair

Hair and fur grown from the faces of a mesh. A `Hair` component sits beside the scalp object's
`MeshRenderer`, scatters strand roots over that mesh's triangles, grows each strand to an authored
length, and shapes the groom with a handful of pen-drawn guide curves.

Thickness spans the whole range on purpose: `thickness: 0.001` with a high density is fine human
hair or fur, `thickness: 0.03` with `shape: tube` and a density of a dozen is the chunky stylised
hair a cartoon character wants. One system, one set of parameters.

| File | Purpose |
|---|---|
| [`hair_params.h`](hair_params.h) / `.cpp` | `HairParams` — every authored key, `sanitize()`, and the content `hash()` that `regenerate: on_change` compares. Nothing derived. |
| [`hair_data.h`](hair_data.h) / `.cpp` | The groom, as pure functions over pure data: `scatter_roots()`, `grow_strands()`, `build_geometry()`, `write_strand_vertices()`, `strand_lod_order()`, plus `ScalpMesh` and the CPU-sampled `MaskImage`. No scene, no device, no asset. |
| [`hair.h`](hair.h) / `.cpp` | The `Hair` component and its YAML parser. |
| [`hair_system.h`](hair_system.h) / `.cpp` | Order **64**: generate, cache, publish, keep the material in step. Owns everything derived. |

## Authoring

```yaml
- name: Head
  components:
    - type: MeshRenderer          # the scalp
      mesh_path: props/head
      material: materials/skin
    - type: Hair
      shape: ribbon               # ribbon | tube
      density: 20000              # strands per SQUARE METRE of scalp
      max_strands: 8000
      length: 0.12                # metres
      thickness: 0.0012
      thickness_over_length: [[0, 1], [1, 0.25]]
      droop: 0.5
      clump: 0.3
      guides: Guides              # a child object; its descendants' Curves shape the groom
      material: materials/natural/hair_brown
```

Only `shape` and the growth keys have to be there; everything else has a default that describes
short fine hair. The full key table is in the `create-scene` skill's `reference/components.md`.

## The four decisions that shape this module

### 1. Density is per square metre, and the scatter is area-weighted

`density` x the scalp's area is the strand count, capped by `max_strands`. Picking triangles
uniformly instead would crowd hair wherever the mesh happens to be subdivided, so a head that was
remeshed would silently grow a different groom. Area weighting is what makes `density: 20000` mean
the same thing on a coarse scalp and a fine one, and on a head and a bear.

Every random choice comes from `hash01()` of (seed, strand index, purpose) and nothing else — the
same determinism contract procgraph's bakes keep. One seed is one groom, on every run, thread and
machine.

### 2. Strands are inextensible

Droop, curl and clumping all pull on a strand, and applied naively they would stretch it. The last
step of `grow_strands()` walks the shaped direction field laying down segments of exactly
`length / segments`, which restores the authored length while keeping the shape — and hands the
solver uniform rest lengths for free.

### 3. The geometry is CPU-baked, width included — not camera-facing

A ribbon is oriented by a frame transported along the strand, not turned to face the viewer. That
is forced by the renderer, not a preference: the shadow backbones
(`assets/shaders/gfx/surface/shadow_vs.glsl`) declare only `in_position`, `in_uv` and `in_model` —
**no tangent** — so a camera-facing expansion in the vertex shader could not produce a matching
shadow, and a groom would cast the shadow of geometry nobody can see.

`cross_section: 2` is the answer to the edge-on problem instead: two crossed planes per strand,
the same trick the tree's leaf cards use, with no camera dependency at all. Their normals lean
**outward from the strand's axis** rather than sitting flat on the plane, because a crossed card
whose plane faces the light goes black — the bug the leaf cards hit, fixed there the same way.

`shape: tube` extrudes a real `sides`-gon: fully opaque, no alpha test, no sorting.

### 4. Everything derived is keyed, never pointed at

The editor destroys and recreates a component on every inspector keystroke, so the strand set, the
built mesh and the published child all live in `HairSystem` under `Hair::state_key()` — the owner's
**name**. Nothing is keyed by a `Component*` or a `SceneObject*`: a published root outliving the
component that made it is exactly how procgraph once produced a use-after-free. The runtime child
is found each time by walking the owner's children for the marker, not by remembering where it was.

## Guides

**Guides are optional.** A groom with none is purely procedural — `droop`, `curl`, `clump` and
`frizz_degrees` shape it — and that is the default. Guides are how you take control when that
shape is not what you want, not a thing every groom needs.

`guides:` names an object — a child of the scalp by preference, otherwise anywhere in the scene —
and **every `Curve` component under it is a guide**. No new editing tool was needed for that:
`Curve` already holds one path in its object's local space, and Pen Mode already edits exactly
that, so a groom of N guides is N child objects. The Hair inspector's **Guides** section lists
them and is where they are added, duplicated and deleted; drawing them is Pen Mode, below.

### Editing guides

Guides are ordinary `Curve` objects, so they need no mode of their own — **Pen Mode** edits them.

| | |
|---|---|
| Enter | select the groom, pick **Pen Mode**. It lands on a guide, creating the first one if there is none |
| Switch guide | with the **Select** tool, click any curve's line. Every curve in the scene is drawn while the mode is open, so they are all clickable |
| Add / Duplicate / Delete | the Hair component's **Guides** inspector section, which lists them with their point counts |
| Draw and reshape | click to add, drag points and handles, G/R/S/E, X deletes, C closes |

Entering Pen Mode on a Hair object redirects to a guide rather than adding a `Curve` to the
scalp: a Hair component holds no curve, so editing the object you selected would edit nothing.
Switching is not hair-specific — the Select tool moves the Pen to whichever curve you click,
which is the same thing a scene full of unrelated paths wants. Deleting the active guide moves
the Pen to a neighbour; deleting the last detaches it rather than leaving it on a destroyed
object.

What a guide contributes is its **shape**, not its positions: `make_hair_guide()` resamples it
evenly by arc length and records the offsets from its own root, in the frame its own root
direction defines. A strand then takes a weighted blend of its nearest guides' offsets, replants
them on its own root frame, and scales them to its own length. So a guide drawn long and to one
side sweeps short hair to that side without lengthening it or pulling its root off the scalp —
which blending positions directly would do.

Guide curves are snapshotted into the scalp's local space on the main thread, exactly as
procgraph's `{curve: …}` binding does, and both handles are transformed as POSITIONS rather than
deltas. They are content-hashed every frame, so dragging a point re-grows the groom while
`regenerate: on_change`.

## Simulation

`simulate: true` hands the groom's guide strands to the XPBD solver in
[`libs/physxcoopa/physxcoopa/hair/`](../../libs/physxcoopa/physxcoopa/hair/README.md). The default
is off, and off is genuinely free: the mesh is static, uploaded once, and its only motion is the
surface shader's vertex hook.

**Only guides are solved.** `sim_guides` render strands are taken on a stride — spread over the
whole scalp, because the strands were already ordered for even thinning — and registered with
`PhysicsWorld`. Every other strand is bound once to its three nearest guides by inverse-distance
weight, together with **the offset it had from that blend at bind time**. That offset is what
keeps a lock spread out: without it every strand following the same guides would collapse onto one
line the moment the solver moved them. So the solver's cost scales with `sim_guides`, not with
`density`.

Each frame, order 64 (before Physics at 100) pushes the roots in from the scalp's transform; the
world solves after its rigid substeps; and `HairSystem::upload()` — called from
`Engine::upload_dynamic_meshes_()`, the only window where the in-flight frame slot is known —
rewrites the vertices from the live particles, back in the scalp's local space. A simulated groom's
mesh is therefore built with one vertex buffer per frame in flight; a static one keeps a single
buffer. Exactly `ClothRenderer`'s contract.

Wind reaches a simulated groom the way it reaches a cloth: `WindSystem` writes the field onto the
component (pushed in, never pulled) and the system hands it to the solver, scaled by
`weather_wind`. A groom that is *not* simulated ignores that entirely — its shader reads the world
wind itself.

Set `wind_flexibility: 0` on a simulated groom, or the shader sway will fight the solver.

### Tuning it

Every `sim_*` key is pushed into the live solver each frame rather than copied at bind time, so
dragging one in the inspector takes effect immediately and never re-scatters the groom. `sim_*` is
therefore excluded from the geometry hash — only `sim_guides` is part of it, because that one
changes what was built.

**`sim_stiffness` is the control that matters.** It is the fraction of the way back toward the
combed pose per 1/60 s: the defaults keep a groom's shape, and something loose like a tail wants
0.05 or less with a higher `sim_stiffness_falloff` so the tips are free. Hair that looks welded to
the head is almost always this being too high.

### Skinned scalps

A skinned scalp grows on its `SkinnedMeshRenderer`'s **own bind pose**, not on the mesh file. It
has to: `Mesh::build_cpu()` welds and reorders the file's vertices while a `SkinnedMeshSource`
keeps them per corner, so the two do not share triangle indices — and a root recorded against one
would be meaningless against the other. Each root therefore stores the triangle it grew on and its
barycentric position in it, which is exactly what a deforming surface needs to put it back.

## The vertex channels

`coopa::gfx::engine::data::Vertex` is fixed at `{position, normal, uv, tangent}` and the G-buffer
pipeline builds its vertex layout once in its constructor — a surface-shader variant may change
only the shader modules and the cull mode. So a strand's attributes have to fit those four:

| Channel | Carries |
|---|---|
| `position` | the ribbon or tube corner, in the scalp's local space |
| `normal` | outward from the strand's axis — radial on a tube, leaning outward on a ribbon |
| `uv` | `(t root-to-tip, per-strand random)` |
| `tangent` | `vec4(strand direction, +1)` |

**`uv.x` is the only channel every backbone declares**, which is why the sway hook keys off it.

**`uv.y` holds a per-strand random rather than a `v` across the strand's width.** Two reasons, both
forced. A groom is one mesh with one model matrix, so unlike foliage — where each card is an
instance and can hash its own root out of `model` — there is nothing in the transform to tell two
strands apart; without this channel a whole head would flutter as one rigid sheet. And the fragment
stage receives only `uv` and the TBN, so a `v` would buy edge alpha while the random buys
strand-to-strand colour and roughness variation, which is what separates a groom into visible
hairs. Width tapering is geometry (`thickness_over_length`); length tapering is the material's
alpha mask sampled by `uv.x`.

**`tangent.w` stays a real handedness.** Every backbone computes `B = cross(N, T) * tangent.w`, so
smuggling a payload there would corrupt the bitangent of the frame the fragment hook reads.

## Rendering, and the highlight

Geometry is published as a `RuntimeObject`-marked child of the scalp with its own `MeshRenderer`,
built in the scalp's **local** space with an identity child transform — so the scalp's world matrix
applies exactly once. (Procgraph publishes world-space geometry instead; this module deliberately
does not repeat that, because a groom must follow a head that moves.) Shadows, frustum culling,
LOD, debug views and the surface shaders then all work with no new render code.

`assets/shaders/hair_surface.glsl` is the vertex hook: the wind sway, shared verbatim by
`hair.vert`, `hair_shadow.vert` and `hair_shadow_cube.vert` so a groom's shadow sways with it.
`hair_surface_frag.glsl` is a **separate** file because it reads the camera UBO, which only the
G-buffer backbone declares.

**Hair's specular is an approximation, and a deliberate one.** Real hair is an anisotropic BSDF
(Kajiya-Kay, Marschner) whose highlight is a band across the strand. A deferred G-buffer cannot
express that — there is no channel for a tangent direction or an anisotropy strength, and adding
one would widen an attachment for every pass in the engine for one material's benefit. The
fragment hook instead turns the shading normal toward the direction a round strand's surface faces
the viewer from (the view vector with its along-strand component removed). Every point along one
strand then presents the same cross-sectional normal, which is exactly the condition that makes an
isotropic highlight stretch into a band. The turn is partial, so the baked per-strand variation
survives.

## Level of detail

Strands are emitted into the **index** buffer in a bit-reversed order, so any prefix of it is a
groom thinned evenly rather than a bald patch. Each LOD level is one such prefix at half the
strand count of the one before, sharing a single vertex and index buffer — so the levels cost no
memory and no second generation pass. Vertices stay in natural strand order, which is what lets a
simulated groom rewrite strand `i`'s block at `i * vertices_per_strand()` and touch nothing else.

Levels do **not** widen the surviving strands to compensate: width is baked per vertex and every
level reads the same vertices, so a distant groom genuinely thins. That is the trade for free LOD.

## Limits

- **On a skinned scalp, only a SIMULATED groom follows the deforming surface.** Its guide roots
  are recomputed each frame as the same barycentric point on the same triangle, wherever skinning
  has put that triangle's corners — a few dozen interpolations, because every other strand follows
  the guides anyway. A *static* groom on a skinned scalp rides the object's transform instead, so
  it follows a head that moves but not a face that deforms; making that exact needs the hair mesh
  skinned alongside, which is the GPU deform path.
- Hair on a rigid part parented to an animated bone — which is what most character heads are —
  needs none of that and has always worked: the groom rides its owner's transform.
- **The scalp's triangles are parsed by this system, not by the asset manager.** `AssetManager`
  keys a slot by the file's RESOLVED PATH rather than by (path, type), so a second CPU-only asset
  type loaded from the same mesh file is refused outright — "already loaded as a different asset
  type", the same one-path-one-type rule that stops a static mesh also loading as a skinned one.
  `HairSystem` therefore parses the file and caches it under the resolved path, keyed on the write
  time so an edited scalp re-grows its hair. One parse per unique scalp mesh, on the main thread,
  the first time a groom is built.
- **`root_mask` is read on the CPU**, through `toyengine/world/png16.h`'s `read_png_rgba8()`,
  because the scatter is a CPU pass and a loaded `data::Texture` has no host copy to sample.
- **A mask thins by rejection**, so masking reduces the strand count rather than redistributing it.
  That is what keeps the distribution even instead of biasing toward the mask's bright edges.
- **No groom brush.** Density, length and direction are parameters, a painted mask and guide
  curves — there is no stroke-by-stroke combing tool.
