# toyengine/mapgen

Whole generated worlds, and the act of putting one onto a terrain.

A terrain does not generate itself. A **`MapGenerator`** component sits on its own object, owns a
[mapcoopa](../../libs/mapcoopa) `MapConfig` and the `MapGraph` it produces, and **applies** that
world to a terrain. Applying writes the terrain's own data and then steps out of the way — the
same gesture as importing a heightmap PNG, and with the same consequence: from then on the
terrain is the authority, and nothing about drawing, streaming or editing it comes back through
here.

```yaml
- name: world
  components:
    - type: MapGenerator
      target: terrain          # the object to apply to; empty = a terrain on this object
      auto_apply: when_missing # when_missing (default) | never | always
      seed: 7
      grid_size: 64
      sea_level: 0.28
      natural: { mountains: 0.45, hills: 0.05 }
      world_tiles: 320         # how finely the map is written into a TILES target
      height_scale: 32.0
```

| File | Purpose |
|---|---|
| [`map_generator_component.h`](map_generator_component.h) | `MapGeneratorComponent` — the config, the async generate, the `MapGraph`, and `apply_to()`. Plus `MapGeneratorSystem` (order **59**) and the `MapGenerator` parser. |
| [`terrain_generator.h`](terrain_generator.h) | The `generator:` block ↔ `coopa::maps::MapConfig`: `default_generator()` (mapcoopa's standard map in its natural terrain model), `generator_config_from_node()`, and `generator_node()`, which writes back only the differences. |
| [`terrain_sampler.h`](terrain_sampler.h) | `TerrainSampler` — the bridge from a `MapGraph` to per-point queries. Adds the point→cell lookup mapcoopa has no need for (a nearest-site query over a uniform bucket index), and answers in two shapes: `sample_record()` quantised into a terraced column, `sample_surface()` continuous in metres. |

## Applying, in both terrain modes

The map is the same either way. Only the resolution it is written at differs — which is what
makes "apply a generated world to a *smooth* terrain" possible at all.

| Target | What is written |
|---|---|
| `TerrainMode::Tiles` | Ground in whole steps, surface layers, biome, climate, region / country ids, and the sea, lakes and rivers. `TerrainSampler::bake()` — the bake a tile world has always had. |
| `TerrainMode::Heightmap` | The float height plane, splat weights from the surface and biome, and the same water field over it as a level per tile. |

**Why there are two sampling shapes.** A terraced column is `sample_record()`'s job and encodes
its height as a step count. That cannot serve a height plane: fine heights would need
`height_step ≈ height / 30000`, and `TileRecord::water_depth` is a single byte — an ocean does
not fit in 255 fine steps. So the smooth path reads `sample_surface()`, which returns metres and
a water surface directly, before any quantisation happens.

## Existing scenes did not have to change

Every tile scene written so far puts its `generator:` block on the terrain, and a bare
`TileTerrain` with no block at all has always meant mapcoopa's standard map. So the terrain
parser **synthesises** a `MapGenerator` beside every tiles-mode terrain, carrying whatever block
the scene wrote ([`toyengine/scene/register.cpp`](../scene/register.cpp)). With
`auto_apply: when_missing` that reproduces the old behaviour exactly: bake when there is no
stored `.tiles`, and otherwise leave the stored world alone.

`TerrainComponent::generator` is still parsed and still saved, because the editor's World
Generator reads it back to tell when a re-parse has landed. It is not what generates.

## It is also a queryable world map

A generated world holds far more than ground: nations and provinces with invented languages,
settlements with streets and plazas, roads, landmarks and multi-level cave systems. Gameplay
reads those through `graph()`, which is why the component outlives the apply instead of being a
one-shot tool.

A terrain keeps the applied view of its map in `TerrainComponent::applied_map` (null on a terrain
that was sculpted or imported rather than generated), so code holding only a terrain can still
reach the world behind the ground:

```cpp
if (terrain->applied_map != nullptr) {
    const coopa::maps::MapConfig& config = terrain->applied_map->config();
    const coopa::maps::MapGraph& graph = terrain->applied_map->graph();
    // ...settlements, regions, rivers
}
```

That is how cubeworld and habitat derive their towns from the island they are standing on.

## How a frame moves through it

```
MapGeneratorSystem::execute()        (order 59, owner thread)
├─ is any terrain asking this generator for a world?   (auto_apply + needs_bake)
│    no, and nothing in flight ──► idle: generation is the expensive half
├─ begin()   ──────────────────►  MapGenerator::generate_async()   [job engine]
├─ poll()                          the graph lands and is held
├─ poll_apply()                    a finished bake lands in the terrain
└─ apply_to(terrain)  ─────────►  TerrainSampler::bake() / the smooth write  [job engine]
```

Order **59** is just before `TerrainSystem` (60), so a world that lands this frame is already in
the tiles by the time the streamer decides what to mesh — the first chunks are never built from
an empty grid. The terrain reports progress itself through `TerrainComponent::baking()`, which is
how the editor shows *Regenerating…* without knowing generators exist.

## Threading

`TerrainSampler::build()` runs on the owner thread; afterwards every member is read-only and
every query is `const`, so the bake's workers sample it concurrently — which is the whole point,
since sampling is most of what a bake does. The sampler is held by `shared_ptr` and captured by
the bake job, so it cannot go while a job is still reading it. An apply in flight holds a
`JobHandle`, and the component's destructor cancels, waits and closes it: every handle is closed
exactly once.

## Not here yet

Streaming generation (chunks outside the baked area generating from the map as the camera moves),
caves and overhangs as geometry, and rivers / roads / towns as 3D geometry rather than as fields
written into the terrain. Each sits on top of this layer rather than requiring it to change shape.
