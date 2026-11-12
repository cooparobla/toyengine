# Caves

Caves are the one feature below the map rather than on it, and that shapes how they are
placed, grown and stored. All keys live in the `caves:` block of
[`assets/config.yaml`](../assets/config.yaml); the pass is
[`passes/pass_caves.h`](../coopa/maps/passes/pass_caves.h).

## Where they open

On the steepest ground. In a Voronoi map that is an edge with a large height difference
across it. Every land edge is ranked by `edge_grade()` (rise over run, in real metres), and
the steepest are taken subject to `min_spacing_m`. `min_grade` (`--cave-grade`) is a floor
beneath that ranking, not the selection itself.

## How they grow

Two regimes, as in real limestone:

| | Above the water table (vadose) | At and below it (phreatic) |
|---|---|---|
| Water is | falling under gravity | moving sideways through saturated rock |
| Passage | steep, narrow | level, wide |
| Branching | rare (`branch_chance_vadose`) | common (`branch_chance_phreatic`) |
| Also | occasional vertical shaft | chambers, maze |

The water table is a subdued copy of the surface: `vadose_share` of the relief between the
mouth and the sea, not a flat sheet at sea level.

## Storeys

A water table drops over time as the valley it drains to cuts down. The network at the
higher level is left dry and a new one forms below. `level_spacing_m` is the drop between levels.
How many levels a system gets (up to `max_levels`) comes from the relief beneath its mouth.
Levels are joined by the same vadose descent that cuts the entrance series.

`level_spacing_m` must stay well clear of `chamber_height_m`, or two storeys touch where
they cross.

## They never break the surface

Every station is clamped to `roof_clearance_m` below the ground. The clamp is applied twice:
once as the station grows, and again on every point of the smoothed passage, because
smoothing moves points. `test_caves_stay_under_the_terrain` checks the smoothed points.

The clamp is against `elevation_at()`, with terrain detail and river channels applied. It is
not against `_elevation.png`: the rendered layer draws a pixel with whichever cell's wobbled
outline contains it, so it can differ from `elevation_at()` near cell edges. Compare a cave
floor against `elevation_at()` if you need an exact answer.

## Storage

Caves are not rasterised. A branching network at several depths does not fit a stack of
heightmaps without being flattened and quantised, and it would cost more: floor/roof PNG
pairs per storey measured 6.9 MB across 10 files, against 873 KB for the same caves in YAML
at full precision.

`save_map()` writes every system: each station with position, floor, roof, radius, cell,
zone, feature and storey, and each passage as the smoothed polyline that gets drawn. Read
the YAML for cave geometry and `_caves.png` to look at it.

The composite shows only what is on the surface: a ring at each cave mouth.
`test_only_cave_mouths_reach_the_surface_layers` checks that every composite pixel the caves
change lies within a mouth marker.

## Main keys

| Key | Shipped | Effect |
|---|---|---|
| `cave_count` | 18 | Systems to open, clamped to the qualifying slopes |
| `min_grade` | 0.25 | Rise over run a slope needs; a floor beneath the ranking |
| `max_depth_m` | 260 | How far below its mouth a system may reach (`--cave-depth`) |
| `roof_clearance_m` | 25 | Rock left above every ceiling |
| `passage_height_m` | 8 | Floor to ceiling in ordinary passage |
| `vadose_share` | 0.6 | Share of the relief below a mouth that is entrance series |
| `level_spacing_m` | 38 | Drop between storeys |
| `max_levels` | 4 | Cap on storeys per system |
| `descent_grade` | 0.15 | How steeply the entrance series falls |
| `massif_bias` | 0.6 | How hard a passage steers toward thicker rock |
| `max_nodes` | 900 | Hard cap on stations per system |
