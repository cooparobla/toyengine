# Map Generation Passes (`coopa::maps`)

Each header here holds one stage of map generation. A pass is a stateless class with a
single method:

```cpp
void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const;
```

`MapGenerator::execute_passes_()` runs them in the fixed order below, each gated by its
`MapConfig::enable_*` toggle. The order is a dependency chain, not a preference — valleys and moisture
need rivers, biomes need moisture and temperature, regions need biomes, towns need regions
to be named in the right dialect, and landmarks need towns to know which land is already
settled. Turning a pass off leaves the fields it would have written at their defaults
rather than reordering anything.

The graph's geometry is built before any pass runs and none of them change it; a pass only
annotates cells, corners and edges.

That single fixed signature is also why `generate_async()` checks for cancellation
*between* passes rather than inside one: threading a token through fourteen `execute()`
methods would change the contract every pass is written to, to shave at most one pass off
the latency — and the longest pass is roughly 50 ms. Nothing in here runs in parallel
either. Generation is about 140 ms against some 10 s of export, so there is nothing to win,
and the road pass could not be parallelised regardless: each route is deliberately routed
over ground the earlier ones already claimed. Every pass that draws randomness seeds a generator from
`MapConfig::seed`, offset per pass so two passes never share a stream.

---

## Pass Order

| # | File | Writes | Reads |
|---|------|--------|-------|
| 1 | [`pass_water.h`](./pass_water.h) | `water`, `ocean` on corners and cells | the island noise field, the `border` flag |
| 2 | [`pass_coast.h`](./pass_coast.h) | `coast`; refines corner `ocean`/`water` | pass 1 |
| 3 | [`pass_elevation.h`](./pass_elevation.h) | `elevation`, `downslope`, `water_level` | pass 2 |
| 4 | [`pass_temperature.h`](./pass_temperature.h) | `temperature` | pass 3 |
| 5 | [`pass_rivers.h`](./pass_rivers.h) | `river` on corners and edges; `MapGraph::rivers` | pass 3 |
| 6 | [`pass_valleys.h`](./pass_valleys.h) | rewrites `elevation` and `downslope` | pass 5 |
| 7 | [`pass_moisture.h`](./pass_moisture.h) | `moisture` | pass 5 |
| 8 | [`pass_biomes.h`](./pass_biomes.h) | `MapCenter::biome` | passes 4, 6 and 7 |
| 9 | [`pass_roads.h`](./pass_roads.h) | `MapEdge::road`, `road_class`, `traffic`, `bridge`; `MapGraph::roads` | passes 5, 6 and 8 |
| 10 | [`pass_regions.h`](./pass_regions.h) | `MapGraph::regions`, `countries`; cell `region`/`country` | passes 6 and 8 |
| 11 | [`pass_towns.h`](./pass_towns.h) | `MapGraph::towns` | passes 8, 9 and 10 |
| 12 | [`pass_landmarks.h`](./pass_landmarks.h) | `MapGraph::landmarks` | passes 8, 10 and 11 |
| 13 | [`pass_caves.h`](./pass_caves.h) | `MapGraph::caves` | passes 6 and 10 |
| 14 | [`pass_noisy_edges.h`](./pass_noisy_edges.h) | `noisy_points0`, `noisy_points1` | pass 8 |

---

## Notes Per Pass

### 1. Water ([`pass_water.h`](./pass_water.h))
The `border` flag it reads comes from `MapGenerator::border_check_()`, which is driven
by `MapConfig::shape` — a canvas-spanning rectangle by default, or a circle, triangle,
continent or archipelago inscribed in the canvas. That one predicate (`ShapeField::inset()`)
is where the shape of the world comes from; nothing in here knows shapes exist.

Threshold the island noise field per corner, promote cells whose corner count crosses
`threshold_water_count`, then flood-fill from the border to mark `ocean`. That last step
is the only thing separating the sea from an inland lake — both are `water`, only the sea
reaches the edge of the map.

### 2. Coast ([`pass_coast.h`](./pass_coast.h))
A land cell adjacent to ocean is coast. Corner state then follows from the cells it
touches.

### 3. Elevation ([`pass_elevation.h`](./pass_elevation.h))
Height is breadth-first graph distance from the border, not a second noise field — which
is what makes coastlines land at sea level and pushes mountains into the interior of a
landmass. Crossing between two land corners costs a full unit, every other step 0.01, so
an inland sea does not raise the terrain around it. Heights are then rank-remapped through
`sqrt(k) - sqrt(k(1-y))` so a continent reads as broad plains with a few peaks.

Between those two steps it blends in fractal relief (`terrain_relief`, default 0.65). The
distance field models *how high* land gets well and *where* badly — the maximum of a
distance field is its medial axis, so untouched it puts every summit on a thin ridge
equidistant between the bays either side, which renders as foam. Mixing toward noise
replaces that ordering; scaling by noise cannot, because distances span tens of units while
a noise factor spans one. A coastal mask keeps the shore the lowest land, and land is lifted
clear of water so drainage still runs seaward. Zero restores the pure distance field.

The rank remap is **split**: the sea takes `[0, sea_level)` and everything else
`[sea_level, 1]`. Ranked together the two would interleave, because a corner far out to sea
accumulates enough hundredths of a step to outrank a coastal one: the sea would have no
consistent depth, the shoreline no consistent height, and `elevation` under water would mean
nothing at all. Split, depth becomes real bathymetry and the waterline becomes a definite
height. Lakes rank with the *land*: a tarn sits at altitude, and the only thing below sea
level is the sea. Smoothing is likewise confined to each side of the waterline, since
relaxing across it drags the sea onto the shore and the shore under it.

Then it **fills the pits**, which is what makes the drainage a drainage. The distance field
is monotone, but nothing after it is: relief noise, the rank remap and both smoothing passes
move corners independently of their neighbours, and any of them can leave a corner lower
than everything around it. A downhill walk that reaches one stops on dry land — unfilled, 80
of 11 438 land corners are such pits, enough to end 23 of 55 rivers in the middle of a field.
The cure is the priority-flood fill (Barnes, Lehman & Mulla 2014), what DEM processing uses for
exactly this: seed a min-heap with every corner already at or in water, then pop the lowest
and raise each dry neighbour to just above it. That walks outward from the sea in ascending
order of the height water would need to reach a corner, so every corner is resolved *from* a
strictly lower one and inherits its descending path to water. Lake corners seed the heap
alongside the sea, so an inflow that reaches a lake has arrived and no lake bed is filled in
to force the water over its rim — which is what keeps an endorheic basin a lake. The epsilon
is 1e-7 and chains run to 39 corners, so the worst-case rise is 4e-6 of the range: about two
millimetres at the default 600 m, and not a visible change to the terrain.

`assign_downslopes()` compares strictly (`<`). An equal-height neighbour is not downhill, and
accepting one would let two corners at the same elevation name each other as their downslope — a
two-cycle a flow walk follows until its step guard. Strictness is also what makes "points at
itself" mean "has nowhere lower to go" rather than "happened to be scanned last".

Finally it levels the water. `elevation` is the height of the *ground*, which under a water
cell is the bed; `MapCenter::water_level` states the surface, which unlike a bed is flat.
`sea_level` across the whole sea, and one height per lake, found by flooding the
`water && !ocean` cells and taking the highest bed in each body so no basin pokes through
its own surface. It has to happen here rather than in the water pass, which is where sea and
lake are told apart but which runs first and has no heights to level against.

### 4. Temperature ([`pass_temperature.h`](./pass_temperature.h))
Latitude band, minus an altitude lapse rate, plus a noise field, plus a global offset.
Without it biomes are classified on elevation and moisture alone, which leaves a third of
the table unreachable and puts deserts at the pole. Latitude runs along **y**, so a map
reads as a north-south slice of a globe.

The band names its polar caps rather than implying them. A plain `1 - d^falloff` does
produce caps — at the default exponent the ground freezes beyond 87% of the way to the pole,
the outer 6.5% of the map — but nothing in the configuration would say 6.5%, and no value of
the exponent says *zero*. `polar_extent_north` and `polar_extent_south` say it
outright, one per pole, anchored to `k_biome_frigid` so the number means the fraction that
actually classifies as frozen. Set one to 0 and that cap vanishes; the curve then spans
freezing to equatorial across the whole hemisphere, so latitude alone never picks ice.
Altitude still can, which is what should happen to a mountain.

`temperature_offset` shifts every sample, so an ice age and a hothouse are one number apart
on the same map. `temperature_falloff` shapes only the temperate half of the curve.

### 5. Rivers ([`pass_rivers.h`](./pass_rivers.h))
Sample sources uniformly, reject any outside the source elevation band, and walk
`downslope` until the water. The attempt count is bounded, so a map with no qualifying land
logs a shortfall rather than hanging.

**Every river ends in a water body**, and that is a guarantee rather than a tendency. The
walk stops on `water || coast` — the union, not `coast` alone, because `PassCoast`
deliberately excludes the shoreline from `water` so the two can be told apart: a sea mouth
is a `coast` corner and a lake mouth a `water` one. Stopping on `coast` alone would also run
a river on down a lake *bed* to its lowest corner, drawing a channel across the surface,
since lake corners sit below their shore. A walk that somehow still ends dry has its river
discarded and logged. With the pits filled that rejection never fires, and it stays anyway:
"always ends in water" is a property callers rely on, and one enforced only by an invariant
two passes away is one a future change to elevation can quietly break.

The walk gathers the corner chain *before* raising any volume, and only commits a
watercourse at least `river_min_length` corners long. Raising volumes while walking would
make a short river impossible to reject, because by the time you can measure it you have
already carved it. Most land is near a coast, so without the rejection the map fills with
two-cell trickles. The source floor (`river_source_min_elevation`, 0.45) is there for the
same reason: much lower and a "source" can appear on the plain it is meant to run down to.

Each kept watercourse is then traced into a `MapRiver` — the corner chain, and a copy of it
corner-cut by `chaikin_smooth()` ([`../map_data.h`](../map_data.h), shared with the road
pass). Drawn straight between Voronoi corners a river is visibly angular at every corner,
which is the one shape moving water never has; corner-cutting rounds the joints without
straightening the course, because it never moves a point more than a quarter of a segment.
The meander is the downslope chain itself and survives intact.

The water *surface* a river is drawn at is not decided here — it is
`make_river_surfaces()` in [`../map_data.h`](../map_data.h), computed for the whole network
after generation, because "only ever falls", "meets the sea" and "agrees at a confluence" are
none of them properties a single segment can check.

### 6. Valleys ([`pass_valleys.h`](./pass_valleys.h))
The only pass that rewrites `elevation` after pass 3, and it has to be: rivers erode, but
elevation is computed *before* rivers are routed — it has to be, the routing follows
`downslope` — so without this the height field has no idea a river pass ever ran, and the
elevation layer would show no trace of the rivers the water layer is full of.

It cuts a **valley, not a channel**, and the geometry forces that. `elevation_at()`
interpolates *cell-site* heights over Delaunay triangles while rivers run along Voronoi
edges — cell *boundaries* — so lowering corner heights alone would change nothing anyone
can see. The cells have to move, and the narrowest thing they can express is about a cell
across. Which is the right answer anyway: a river sits in a valley far wider than itself,
and the channel within it is what the water layer draws.

Depth per corner from `river_incision_m` plus `river_incision_per_volume_m` × volume,
tapered over the first few corners so a river does not begin with its valley already cut.
Graded outward over `river_valley_width` rings of `adjacent` at `river_valley_falloff`
each — `max`, not `+`, so a confluence is one valley rather than two stacked. Corners are
then lowered, clamped at the waterline, and the descent along each course is restored;
cells take the *mean* of their corners' depths, which applies the valley on top of
`smooth_center_elevations_()` instead of throwing that relaxation away.

It finishes by calling `restore_drainage()` ([`drainage.h`](./drainage.h), shared with
pass 3). Moving the ground owes the map its drainage back: carving digs pits where a
valley wall grades into ground with no outlet, the waterline clamp flattens river mouths
into ties, and `downslope` was read off a field that no longer exists.

Setting `river_incision_m` to 0 reproduces the uncarved height field byte for byte, which
is what the layer test asserts.

### 7. Moisture ([`pass_moisture.h`](./pass_moisture.h))
Lakes and rivers seed the field; the ocean deliberately does not, so a desert can sit
behind a coastal range. Diffuse outward losing a tenth per hop, pin ocean and coast wet,
then rank-normalise so every map spans the full range.

### 8. Biomes ([`pass_biomes.h`](./pass_biomes.h))
Delegates to `classify_biome()` in [`../biome.h`](../biome.h), which branches on
temperature first, then elevation, then moisture — about 33 biomes rather than the
original 18.

**A water cell always gets a water biome** — `Ocean`, `Lake` or `Ice`, and nothing else.
This is not cosmetic. The biome and composite layers are coloured by *biome*, not by
`MapCenter::water`, so the two disagreeing is a defect the reader sees however sound the
data underneath is: a shallow lake classified `Marsh` (33, 94, 33 — a dark green) would end
rivers in what looks exactly like forest. A lake you cannot see is indistinguishable from no
lake, and rivers must visibly end in water.

`Marsh`, `Swamp` and `BorealWetland` are land biomes, which is what the words mean:
waterlogged basin floor beside the water rather than the water itself. Moisture is seeded
from lakes and rivers, so they land where they belong.

Freezing is on temperature alone. An elevation test that also froze a high lake would
double-count altitude, because `PassTemperature` already applies an altitude lapse rate —
a high lake would be frozen twice over and a cold low one not at all.

The thresholds are fractions of the **land** range, because the pass passes
`land_height()`, not absolute heights: a threshold like `0.1` means "the bottom tenth of the
land", which is where lakes sit, since a lake is a basin. A table test on `classify_biome()`
alone cannot catch a mismatch here, because it is the arguments that are rescaled and not
the function.

### 9. Roads ([`pass_roads.h`](./pass_roads.h))
Roads are *routed*, not drawn. Every Delaunay edge gets a travel cost from the ground
either side of it — distance scaled by slope, height and how rough the biome is, plus a
volume-scaled charge to ford or bridge a river and a high but finite one to step into
water. Anchors ("hubs") are picked with `biome_habitability()`, and a least-cost path is
run between **every pair** of them.

Routes are laid one at a time, longest link first, and each sees ground an earlier route
already built on as `RoadConfig::reuse_discount` times as dear. That one rule is what makes
this a network rather than a fan of independent optimal paths: a later route bends to join
an existing road instead of paralleling it a cell away, trunks consolidate, and traffic
concentrates. An edge's `traffic` is the number of routes that chose it, and `RoadClass`
is a pair of thresholds on that count taken as a *share of the routes laid* — a dead-end
spur always carries exactly `hubs - 1` routes, so an absolute cutoff would mean something
different on every map.

Switchbacks emerge for a reason rather than by construction: `slope_cost` makes climbing straight up dear and traversing a slope cheap, so
a mountain route crosses the contour at a shallow angle and doubles back. Bridges need no
geometry — a road follows the Delaunay edge `d0`–`d1` and a river the dual Voronoi edge
`v0`–`v1`, and those are the same `MapEdge`, so an edge carrying both *is* the crossing.
Finally each maximal same-class chain is traced into a `MapRoad` and corner-cut with
Chaikin, ends pinned, because a path drawn straight between cell sites is visibly faceted
and no road is.

Water is crossable but bounded: a causeway may span up to `max_water_span` consecutive
water cells, so nearby islands link up while the open ocean stays impassable and a remote
island keeps its own self-contained network. The search state is therefore
`(cell, consecutive water crossed)` rather than just the cell — the same lake cell is
reachable one hop from shore and unreachable three hops out.

**Why routed rather than contoured.** Flagging every edge whose corners fall in different
elevation bands traces contour lines, and contour lines connect nothing: a road could run
half the map without passing a settlement, every road would be the same width, and
`TownConfig::road_bonus` would reward proximity to a contour rather than to a trade route.

**Why it runs before towns.** Towns want roads to score sites by and good roads want towns
to connect, which looks circular. It is not: the pass picks its own hubs with the same
`biome_habitability()` table `PassTowns` ranks sites with, so it depends only on biomes and
rivers, and the settlements placed two passes later land on the network because both
passes read the same ground. The pass draws no randomness at all — terrain decides
everything, so there is no stream to seed.

### 10. Regions ([`pass_regions.h`](./pass_regions.h))
Scatter country seeds across the land, then claim territory by multi-source Dijkstra where
climbing is expensive and crossing water more so — which is why the borders that emerge
follow ridgelines and coasts instead of cutting across them. Each country is subdivided the
same way, and any land the fill could not reach is adopted by its nearest claimant so no
cell is left stateless. Countries draw a synthetic language; regions get a dialect of it.

### 11. Towns ([`pass_towns.h`](./pass_towns.h))
Score land cells on biome habitability (`biome_habitability()` in
[`../biome.h`](../biome.h), the same table the road pass ranks hubs with), low ground, and
access to sea, river or road;
accept the best greedily subject to a minimum separation so settlements spread across a
continent rather than clustering on one river mouth; then rank them into capitals, towns
and villages.

A settlement is **not one cell**. At 60 m to the grid unit a cell is about 3,600 m², which
holds roughly ten 10 m buildings at a believable density — so a capital confined to its own
cell is a hamlet and the three tiers stop being distinguishable. Each settlement claims
cells breadth-first over `MapCenter::neighbors`, taking dry habitable land no other
settlement holds: seven for a capital, three for a town, one for a village. Breadth-first
and not a radius, so the claimed patch is contiguous and a coastal town grows along its
shore instead of reaching across the water. The packer then runs per claimed cell, carrying
the growing building list forward so `buildings_overlap()` still rejects a footprint that
would cross a boundary into ground already built on.

Each accepted cell is then laid out. The roads and rivers bordering it become **streets**
running from the cell's site out to those edges — a road is drawn along the Delaunay edge,
so a road-flagged border means one enters the cell and heads for its centre. A cell with
neither gets fallback lanes toward its farthest corners, so even an isolated hamlet has a
lane rather than a scatter. Buildings are placed in pairs flanking each street, walking
outward, jittered in position and yaw so the rows read as built over time rather than
surveyed. Whatever budget remains is spent on rejection sampling across the cell, filling
the interior without falling back into a grid.

Three things make that read as a settlement rather than as noise, and the first is the one
that matters:

- **The streets are emitted.** Kept private to this pass they would be real enough to place
  plots against and invisible to everyone else, so buildings would line up along something
  nobody could see, and a settlement would read as scatter however carefully it had been
  arranged. They are `MapTown::streets`, drawn on the structures layer and written to the
  map file.
- **Interior infill takes the bearing of the nearest street** instead of a yaw drawn
  uniformly from a full turn. That alone puts **83%** of buildings fronting a street, against
  **51%** with random yaws; a row is only legible if its neighbours agree with it. Position stays
  jittered, which is what keeps the layout off a lattice — that is a property of where
  buildings sit, not of which way they face.
- **Buildings are kept off the carriageway.** `can_place_()` tests every candidate against
  the cell's streets and the road polylines passing through it, at half the roadway's width
  plus `street_clearance_m`, and frontage plots are set back by their own rotated size rather
  than a flat `street_offset_m` — which a `building_size_max_m` plot overruns by a metre. Both
  halves are needed: without them 37% of buildings stand on a lane and 18% on a road, and a
  keep-out alone would reject the rows rather than place them. A corridor is tested as a rotated box
  through `buildings_overlap()`, because a corner-distance test misses a lane crossing the
  middle of a plot.
- **A market square, and roles.** A settlement claiming at least `plaza_min_cells` gets a
  `MapPlaza` at its primary site — a disc, trimmed to fit the cell and skipped when there is
  no room — which the packer keeps clear of building. The buildings nearest it are then given
  civic roles from one roster shared by every tier, so a town reads as a smaller capital
  rather than a different kind of place. Everything else is a `Dwelling`.

Every candidate must have all four of its **rotated** corners inside the cell polygon and
must clear every building already placed, by separating-axis test. That is what makes
`MapBuilding::rotation` a pose a consumer can trust.

Names are drawn from the region's dialect and then *claimed*: a name already taken is
redrawn, up to a bound, after which an ordinal is appended. Two settlements of one region
draw from the same small phoneme table, so a collision is a matter of how many towns that
region got — and two places sharing a name silently conflates them for anything keying on
one.

The upstream generator this ports has no working town pass; the scoring, placement and
layout here are mapcoopa's own.

### 12. Landmarks ([`pass_landmarks.h`](./pass_landmarks.h))
Natural features are *read off* the terrain rather than sprinkled onto it — a peak is a cell
higher than all its neighbours, a waterfall a river edge with a real drop across it — so they
always agree with the map they sit on. Ruins go where people could live but do not, giving
the world a past. Every kind is checked against `landmark_suits_biome()`, and no single kind
may take more than a fifth of the budget: ranking alone lets whichever signature happens to
be commonest swallow every slot, and the result is forty hot springs and no coastline.

### 13. Caves ([`pass_caves.h`](./pass_caves.h))
The only feature that exists *below* the map rather than on it, and the first thing in the
module that has two heights at one position rather than one.

**Mouths are ranked, not scattered.** A cave opens in a face of rock, so it belongs on the
steepest ground there is — which in a Voronoi map is a `MapEdge` with a large height
difference across it, already the object that sits *between* two cells. `edge_grade()`
([`../map_data.h`](../map_data.h)) measures rise over run in real metres, and the pass takes
the steepest edges subject to `min_spacing_m`, the same score-sort-accept shape `PassTowns`
and `PassRoads::choose_hubs_()` use. `min_grade` is a floor beneath that ranking rather than
the selection itself: a default map has thousands of qualifying edges and the eighteen that
get mouths are far steeper than the threshold. The mouth sits at the edge midpoint and the
first passage heads from the *lower* cell toward the higher one, because a cave mouth is
something you walk into the hill through.

`edge_grade()` is the module's one measure of slope; nothing else answers it. `hillshade_()` takes a
gradient off the blurred 8-bit raster with a 600× exaggeration baked in, so it answers a
question about a picture; `RoadConfig::slope_cost` uses a height difference never divided by
the distance it spreads over; and `downslope` is a direction with no magnitude.

**Two regimes, which is what limestone does.** Above the water table water falls under
gravity: steep descent at `descent_grade`, narrow, rare branches, the occasional vertical
pitch. At and below it the water moves sideways through rock already full of it and attacks
every joint it meets: level passage, wider, branching several times as often, chambers at
the junctions. One system therefore reads as an entrance series leading to a level network.

The water table is `vadose_share` of the relief between the mouth and the sea, capped by
`max_depth_m` — a subdued replica of the surface rather than a flat sheet, because rain
falls on the hill and drains to the valleys either side. That is not decoration: measured
against sea level alone a cave 400 m up needs 250 m of descent before it can level out,
further than its budget reaches, so every system would come out pure entrance series and
the phreatic half would never appear on a map.

**Storeys, because the table moved.** The valley a system drains to cuts down over time and
the water table follows it, abandoning the network standing at the higher level — left as dry
passage — and starting a new one below. `level_tables_()` returns that sequence, and growth
runs the same two-regime model once per table, joining each to the next with the descent that
*is* a shaft between levels. Nothing about the model is special-cased for it: a descent head
is an ordinary head whose table happens to be the next one down.

The sequence is anchored at the bottom. The deepest table sits where a single table would,
so `vadose_share` and `max_depth_m` say how deep a cave may go whatever the number of
levels, and the abandoned levels are stacked upward from it at `level_spacing_m`. Anchoring
at the top would split one budget of relief between the entrance series and the storeys, so
raising the spacing would make caves shallower — two knobs pulling on one number.

`level_spacing_m` also decides how many storeys a map gets, and not gently: a system needs
that much relief to spend per extra table, so doubling it roughly halves the count. It has to
clear `chamber_height_m` or two storeys intersect where they cross, leaving no rock between
the levels. A descent is given `level_budget` of a trunk *plus* what the climb down
costs, because the climb is overhead rather than network — charged against the same budget it
spends `level_spacing_m / descent_grade` before reaching the level it was sent to dig, and
the lower storeys come out as stubs.

**`massif_bias` is what makes a system reach anywhere.** Each station probes the surface a
step ahead to either side and leans toward the higher one — but *only when the rock ahead is
thinning*. Under a massif every direction has depth to spare and the passage is left to the
meander. Leaning unconditionally makes a head orbit the nearest summit, and a cave drawn as
a spiral is not a cave. Held by the roof clamp alone it wanders out from under its own hill
within a few hundred metres and has to stop, which is stubs.

`noise_cave`'s frequency is set against `step_m`, not against the landforms the other noise
fields shape. A station advances half a grid unit; sampled at a landform frequency the field
barely changes over that, so every station turns by nearly the same amount as the last —
which is a circle.

**It never breaks the surface**, and the clamp runs **twice**: once as a station is grown,
and again on every point of the smoothed passage. Corner-cutting is a convex combination, so
it moves points, and over concave ground a smoothed midpoint can rise above a surface both
its neighbours sat safely beneath. The smoothed path is what gets drawn and exported, so it
is the geometry the guarantee has to hold on — remove the second clamp and
`test_caves_stay_under_the_terrain` fails on the smoothed points alone.

The surface it clamps against is `elevation_at()` **with** `TerrainDetail` and
`RiverChannels` applied, not the control mesh. `river_channel_depth_m` cuts 18 m out of the
drawn surface at its default, so a cave given 25 m of clearance against the mesh has 7 m of
rock over it where it crosses under a river, and daylights outright at a higher `--channel`.

A head that cannot be settled without breaking the system's own floor ends in a `Sump`
rather than being allowed to surface. `Sump` means *cut short by the rock*; a run that
simply spent its budget ends in a `Chamber`, because those are different places and
deserve different names.

**Why it runs here.** It needs the *finished* height field, so after `PassValleys` — the one
pass that rewrites elevation after pass 3 — and it wants regions, so it can name itself in
the local dialect the way `PassTowns` does. Nothing reads caves, so nothing needs it earlier.

### 14. Noisy Edges ([`pass_noisy_edges.h`](./pass_noisy_edges.h))
Redraw each cell boundary as a path that wanders inside the quadrilateral formed by the
Voronoi edge and the two cell sites, recursively, to a depth of three. Both neighbouring
cells read the same edge, so they can never disagree about where their shared border runs.
Cost is paid only where it shows: open ocean is never subdivided, same-biome interiors only
if long, coastlines and river banks always.

Controlled by `MapConfig::subdivide_noisy_edges` — off draws every cell boundary as a
straight line, as the upstream generator does.
