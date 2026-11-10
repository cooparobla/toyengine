# Water, rivers and valleys

Design notes and measurements for the water model. The relevant keys are documented inline
in [`assets/config.yaml`](../assets/config.yaml); the passes are described in
[`coopa/maps/passes/README.md`](../coopa/maps/passes/README.md).

## Sea level and vertical scale

`sea_level` (default 0.25) is a real height in the normalised `[0, 1]` field. The sea bed is
everything below it and land everything above it, so `_elevation.png` carries genuine
bathymetry and the sea's surface shows as mid-grey on the same scale.

| Layer | Shows |
|---|---|
| `_elevation.png` | the ground, sea bed included |
| `_water.png` | the surface of whatever water covers it, same scale |

`MapCenter::water_level` carries that surface as data. It is flat per body: the sea at
`sea_level` everywhere, and each lake at one height across all its cells. A consumer can
flood a terrain mesh to it directly.

Land spans `[sea_level, 1]`, so every threshold that describes land (the biome table rows,
`elevation_penalty_start`, `peak_elevation`, `canyon_elevation`) goes through
`land_height()` first. Moving the waterline does not shift them.

`elevation_range_m` (default 600) is how many metres the field spans. The sea floor is at
0 m, the waterline at `sea_level * elevation_range_m`, and the summit at the full range.

## Rivers end in water

Every river ends in a lake or the sea. That has two halves:

- **Arriving.** A river is a walk down the flow field. Relief noise, the rank remap and the
  smoothing passes can each leave a corner lower than all its neighbours, and a walk that
  reaches such a pit stops on dry land. The elevation pass fills the pits with a priority
  flood, so every land corner has a strictly descending path to water. The river pass also
  discards any river that still ends dry.
- **Showing it.** Water cells only ever get the biome `ocean`, `lake` or `ice`. Wetlands
  such as `marsh` and `swamp` are dry land beside the water.

Other river rules:

- `river_depth_m` (plus `river_depth_per_volume_m`) lifts a river's surface above the
  ground it runs over. It is interpolated along the smoothed centreline, so the fall
  downstream is continuous.
- Lakes and the sea are drawn after rivers, so a lake's flat surface wins inside its own
  outline.
- `water_edge_overlap_m` (default 1) extends every water surface past its edge so it clips
  into the terrain instead of leaving a seam.

## River mouths

`make_river_surfaces()` in [`map_data.h`](../coopa/maps/map_data.h) computes the surface for
the whole network at once. The surface only falls, agrees exactly where two rivers meet, and
arrives at the waterline.

Arriving needs the terrain to move. The ground at a mouth can stand tens of metres above sea
level, so over the last `river_mouth_blend_m` (default 250, about four cells) the channel
becomes an estuary: it deepens until its bed reaches the water it empties into, and widens
with a flat bed. A flat bed matters, because the edges of a wide parabolic channel would
still rest on the bank.

The bed is cut to exactly one freeboard below the target level, so a river ends at or above
the water it feeds, never below it. The one place a surface rises is the last stretch into
a body that stands above it (a drowned inlet). That is bounded to `river_mouth_blend_m` and
never goes above the body's own level.

Measured on one map against a surface computed per segment, without the estuary: ocean
mouths sit a median of +0.0 m above the sea rather than +10.9 m (worst +9.5 m rather than
+100.8 m); no river rises outside the mouth blend, against 48 of 55; and confluences agree
exactly, against a 30.8 m disagreement.

> **Known limitation: lake levels.** A lake's `water_level` is the highest bed in its body,
> which can put its surface above the land around it. On one map, three lakes stood over
> 200 m above the lowest dry ground on their shores. Rivers rise to meet that level, so the
> join is seamless, but the level is not one water would hold. Fixing it means changing how
> a lake's extent is decided.

## River valleys

Elevation is computed before rivers are routed, because routing walks `downslope`. So the
rivers have to be cut into the terrain afterwards. Nothing is painted; the ground is
lowered. That happens at two scales:

- **The valley** is carved into the control mesh (corner and cell heights) by `PassValleys`
  in [`passes/pass_valleys.h`](../coopa/maps/passes/pass_valleys.h). It is a landform
  hundreds of metres across.
- **The channel** is cut into the sampled surface by `make_river_channels()` in
  [`map_data.h`](../coopa/maps/map_data.h), at the river's true width from `river_width()`.

The channel exists because the valley cannot do the job alone. `elevation_at()`
interpolates cell-site heights, while rivers run along cell boundaries. The surface cannot
vary faster than one cell (60 m) against a river 5 to 20 m wide. Measured half a cell from a
centreline, the best mesh carving gives +2.3 grey levels of contrast; cutting the channel
at sample time gives +12.3.

So the channel lives in `elevation_at()`, like `terrain_roughness`, and is never written
back to the graph. Biomes, rivers and roads still classify on the uncut mesh, while anything
sampling the surface (the renderer, a mesher) sees the channel.

| Key | Default | Effect |
|---|---|---|
| `river_channel_depth_m` | 18 | Channel depth in metres. 0 leaves the sampled surface uncut. |
| `river_channel_depth_per_volume_m` | 4 | Extra channel depth per unit of volume. |
| `river_incision_m` | 60 | Valley depth in metres. 0 reproduces the uncarved mesh byte for byte. |
| `river_incision_per_volume_m` | 12 | Extra valley depth per unit of volume. |
| `river_valley_width` | 2 | Rings of corners the valley opens over. 1 is a trench; 2 gives it banks. |
| `river_valley_falloff` | 0.5 | Depth multiplier per ring outward. |

```bash
./build/mapcoopa --incision=0 --channel=0   # the uncarved field
./build/mapcoopa --channel=40               # gorges
./build/mapcoopa --no-valleys               # landform only; the channel stays
```

The valley pass ends by calling `restore_drainage()` from
[`passes/drainage.h`](../coopa/maps/passes/drainage.h), which it shares with the elevation
pass. Carving can dig new pits, so drainage is rebuilt afterwards.

Channel sampling is guarded by a per-cell bounding box, so most pixels in a render cost one
comparison.
