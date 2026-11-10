# nav -- navigation built from colliders

A* pathfinding and flow fields for crowds, over a walkable surface voxelized straight from the
`PhysicsWorld`'s colliders. Fully 3D: stairs, ramps, bridges and stacked floors are ordinary
surface, so "the fastest way upstairs" is just a path query.

```
colliders (PhysicsWorld) ──gather──▶ SourceShape snapshots ──NavBaker──▶ NavMesh (per agent profile)
                                                                            │
                     ┌──────────────────────────────┬────────────────────────┤
                     ▼                              ▼                        ▼
             find_path() (A*)              FlowField::build()          Crowd::update()
             corridor + fine search        Fast Marching + gradient    steering, separation,
             + string pulling              directions                  wall sliding, links
```

## The surface: a tiled, layered voxel grid

Not a polygon mesh. The world is divided into **tiles** (32 x 32 cells by default) and every
cell **column** holds one **span** per floor in it -- the free space above a walkable surface,
with its floor height, ceiling, area id and distance to the nearest wall. Spans **link** to
the span they can step to in each of the four neighbouring columns (floor difference <= the
agent's climb, shared clearance >= its height). Diagonal moves are derived, and allowed only
when both L-shaped routes exist and agree, so paths never clip a corner.

One structure serves both search styles: A* walks it as a graph, flow fields integrate over it
as a grid. Recast builds its polygon navmesh from the same compact heightfield; stopping one
step earlier keeps per-cell flow vectors natural and makes partial rebuilds trivial.

### Build pipeline (per tile, `nav_baker.h` -> `heightfield.h` -> `nav_tile.h`)

1. **Gather** (calling thread): the tile's triangles from every overlapping source (boxes exact;
   spheres and capsules tessellated; mesh colliders culled through their BVH), padded by the
   agent radius + 3 cells so erosion near the edge sees the neighbouring geometry.
2. **Rasterize** (job): conservative triangle-vs-cell clipping into a solid heightfield. Triangles
   steeper than `max_slope` rasterize as unwalkable. The low-hanging-obstacle filter turns curb and
   stair-nosing tops within `max_climb` walkable.
3. **Per agent profile** (job): open spans (clearance >= height), NavVolume area relabelling,
   links, a chamfer distance field from the unlinked boundary, erosion by the agent radius,
   cropping to the tile interior, and tile-local **regions** (connected patches).
4. **Commit** (calling thread): `NavMesh::commit()` produces a new immutable mesh that shares
   every untouched tile with the old one. It **stitches** links across the changed tiles'
   borders (copy-on-write of the neighbours), rebuilds the **coarse region graph** edges,
   resolves off-mesh links and recomputes **connected components**.

Immutability is the concurrency model. Queries and background flow-field builds hold a
`shared_ptr<const NavMesh>`, so a commit landing mid-query can't change what they read.

Incremental updates: `NavBaker::set_sources()` diffs sources by key and fingerprint. Every tile an
added, removed or moved collider touches is marked dirty, rebuilt in the background once it has
been quiet for `rebuild_delay`, and committed in a batch.

## A* (`path_query.h`)

- **Reachability first.** If start and goal are in different connected components, the query
  doesn't flood the world to find out. It plans straight to the reachable span nearest the
  goal (a ring search over the coarse graph) and returns `Partial`, or `NoPath` when
  `allow_partial` is false. One-way links make components conservative, so then it searches.
- **Hierarchical.** For goals more than a tile away, A* over the coarse region graph picks a
  corridor. The fine search is confined to it plus a one-region margin, and retried unconfined
  if a filter closed a gap the coarse graph thought open.
- **Fine search.** 8-connected over spans plus off-mesh links. Cost is 3D step length times the
  span's area cost (and an optional wall penalty), with an octile/3D heuristic (default weight
  1.1; 1.0 is exactly optimal). Node storage is a generation-stamped open-addressing map, so a
  warm query allocates nothing.
- **Smoothing.** Greedy string pulling with a surface raycast that follows steps and floors.
  It never crosses a wall, a link, or a span costlier than the raw path crossed. A final pass
  drops leftover kinks.
- **Batches.** `find_paths()` runs a batch across the job engine with per-thread scratch.

Measured on a 200 x 200 m level with 400 random obstacles (593k spans, 0.25 m cells, 8
workers): full build 45 ms; a batch of 256 random cross-map queries 100 ms on the job engine
(~2 ms each serially, at the default heuristic weight).

## Flow fields (`flow_field.h`)

- **Fast Marching (Eikonal), not grid Dijkstra.** Distance grows isotropically, so on open
  ground the field points straight at the goal instead of along eight compass directions. A
  disk around each goal is seeded with exact distances, which removes most of first-order
  FMM's diagonal error.
- Marches over span links, so a field flows **up stairs and across floors**. Off-mesh links are
  edges in the same march.
- Several goals per field (nearest wins), area costs, a **wall penalty** (default on) that keeps
  crowds off walls, and `max_distance` to bound the march in big worlds.
- Directions: a job-parallel pass computes the negative gradient, falling back to the
  steepest-descent neighbour where the gradient is unreliable (beside walls, on ridges).
  `sample()` blends the four nearest spans bilinearly.
- Immutable and holding its own mesh, so `NavSystem` rebuilds fields in the background and swaps
  them in; agents keep following the previous field meanwhile.

### Open worlds: hierarchical flow fields

Integrating the whole reachable surface costs time in proportion to the world. A crowd only
needs directions where it is, so for large areas a field is built in two layers:

1. **Coarse layer.** Every tile border is cut into **portals** (`NavPortal`): runs of crossings
   between one region on each side, built with the tile at commit time. A Dijkstra from the goal
   over the portal graph gives every portal a cost-to-goal. Edges are straight lines across a
   region, plus the crossings themselves. It stops a margin after every follower's region is
   reached: thousands of nodes and a millisecond or two, whatever the world size.
2. **Active tiles.** Only these are integrated exactly:
   - every tile a follower stands in;
   - `lookahead_tiles` more along each follower's coarse route;
   - every tile within `near_radius` of the goal.
3. **Exact pass.** One fast march over the active tiles, seeded at the goal and at the active
   set's boundary with the coarse estimate of the cell just outside. Directions are consistent
   inside; the rest of the world is summarised by the coarse layer. Coarse costs are scaled by
   the worst-case wall penalty, so boundary seeds never undercut the exact values next to them
   and crowds don't leak out of the exact area. Distances reported away from the goal therefore
   read high.
4. **Coarse fallback.** Outside the active tiles, `sample()` still answers from the coarse layer
   (`FlowSample::coarse`: head for the region's best portal). NavSystem rebuilds the field when a
   follower ends up there, at most every `rebuild_interval`, so the exact window rolls along
   with the crowd.

`FlowFieldMode::Auto` (the default) picks between the two: an exact field when the coarse pass
reached at most `exact_tile_budget` tiles (`nav_test`, any small level, or followers already near
the goal), hierarchical otherwise.

In the `nav_open_world` scene (400 x 400 m, 0.5 m cells, 676 tiles, 246 mobs in seven packs on
two fields), the player's field never integrates more than 38 tiles (5.6%). On a 480 x 480 m
benchmark with 316 followers spread 80 to 220 m out, a rebuild takes 4.5 ms against 59 ms for
the exact field (13x), and the crowd converges just as fast (90 s: 251/316 within 15 m either
way). All planning and marching runs in background slices; kicking a rebuild costs the frame
nothing.

## Crowds (`crowd.h`)

Path following (corner steering with arrival slowdown) or flow following, plus:

- **Separation.** Averaged over neighbours (summed, a crowd's push overwhelms steering) and
  weighted toward neighbours ahead. An oncoming agent straight ahead triggers keep-right.
- **Overlap resolve.** Each agent pushes out of half of any remaining overlap, along the surface.
- **Neighbours on connected surface only.** An agent on the ground beside a staircase never
  pushes one on the stairs (a short surface raycast decides).
- **Arrival contagion.** An agent touching an arrived agent that is closer to the same goal
  arrives too, but only within the group's packing radius. Without it the crowd swirls on the
  goal point. Unbounded, it spreads back down a queue and stops agents that still had room.
- **Off-mesh links** are traversed on a short arc, and movement slides along walls
  (`NavMesh::move_along_surface`).

Each update snapshots positions and velocities and builds a spatial hash, then updates every
agent in parallel, reading only the snapshot and writing only its own slot. It is race-free and
deterministic (5000 agents converging on one goal: ~1 ms per update on 8 workers, of which
~0.3 ms is the parallel step).

Same level, one exact flow field over all 454k reachable spans: 42 ms of march (serial, in Low
background slices) + 15 ms of directions (parallel). In practice NavSystem bounds an exact field
to its followers, and on large areas Auto switches to a hierarchical field (above).

## Real-time rules (how it uses the job engine)

The frame never waits on anything long:

| Work | Where it runs |
|---|---|
| Tile builds (rasterize + per-agent build) | `Low` jobs, one per tile, committed a frame or more later |
| Flow-field rebuilds | `submit_flow_field_build()`: a chain of `Low` slices (4096 spans each), then the direction pass in 4-tile chunks; swapped in when done |
| A* requests | one `Low` job each; results applied when the batch finishes (one or two frames later) |
| Crowd update | `Normal` chunks of 64 agents, `parallel_for_blocking` with a **`Normal` help floor** |
| Collider/volume/link change scan | main thread, every `source_scan_interval` (0.1 s), not every frame |
| First full build and first flow field of a scene | blocking and parallel (loading) |

The help floor matters as much as the slicing. A waiting thread runs whatever job it finds, so
without it, the main thread waiting 0.2 ms for crowd chunks could pick up a whole background
flow-field build. See `JobEngine::wait_for(handle, help_down_to)`.

Work scales with what is used, not with world size:
- **Flow fields** march only until every follower is covered, plus a margin
  (`FlowFieldSettings::required_points`; NavSystem passes the followers). They are rebuilt when
  one wanders out.
- **The fast-marching queue** is a bucketed "untidy" queue: O(1) push/pop instead of a heap.
- **Every span stores its resolved neighbours**, so `NavMesh::neighbor()` is one load. A*, the
  march, raycasts and the crowd all lean on it.
- **The debug overlay** caches mesh and flow lines per mesh/field version.

`NavSystem::stats()` reports the per-stage main-thread cost of the last frame.

## Scene layer

| Piece | File |
|---|---|
| `NavSystem` (order 150: after physics, before Behaviour), `install_nav_system()` | `system/nav_system.h` |
| `NavAgent`, `NavModifier`, `NavVolume`, `NavLink` components | `components/nav_*.h` |
| YAML parsers (registered by `register_physics_components()`) | `nav_yaml.h` |
| `navigation:` settings block | `nav/nav_settings.h` |

The system idles until the scene has a `NavAgent` (or `build_without_agents: true`). Then:
the first frame builds everything, blocking and parallel. After that, collider changes rebuild
tiles in the background. Dynamic rigidbodies are ignored unless a `NavModifier` with
`carve: true` asks them to carve while asleep. Agents are grouped into one `Crowd` per agent
profile. Flow fields are shared per (target object, profile). Path requests are budgeted per
frame and run in parallel.

## Files

| File | Contents |
|---|---|
| `nav_types.h` | `SpanRef`, areas, `AgentProfile`, `NavBuildSettings`, `NavGridParams`, `QueryFilter`, `NavPath` |
| `nav_source.h` | `SourceShape`/`SourceVolume`, `gather_sources()`, shape triangulation |
| `heightfield.h` | solid heightfield + conservative rasterization + low-hanging filter |
| `nav_tile.h` | `NavSpan`, `NavTile`, `build_tile()` |
| `nav_mesh.h` | `NavMesh`: commit/stitch/components, `find_nearest`, `locate`, `floor_height`, `raycast`, `move_along_surface` |
| `nav_baker.h` | `NavBaker`: full and incremental builds on the job engine |
| `path_query.h` | `find_path()`, `find_paths()` |
| `flow_field.h` | `FlowField` |
| `crowd.h` | `Crowd` |
| `nav_debug.h` | debug lines: outline, grid, tiles, links, paths, flow arrows |
| `nav_settings.h` | `NavSettings`, `parse_nav_settings()` |
