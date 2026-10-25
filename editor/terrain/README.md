# editor/terrain

Terrain sculpting and painting tools, as **pure functions** over the engine's terrain data —
no scene, no GPU, so they are unit-tested directly.

| File | Purpose |
|---|---|
| [`terrain_brush.h`](terrain_brush.h) | Heightmap terrain tools over a `HeightmapData`: raise / lower, smooth, flatten, and splat-layer painting. |
| [`tile_brush.h`](tile_brush.h) | `TileTerrain` tools over a `TileMapData`: square brushes (raise / lower / set height, paint kind, water). |

The panels are `../app/ui/terrain.cpp`, `tile_mode.cpp` and `tile_terrain_panel.cpp`; the engine
side is [`toyengine/world/`](../../toyengine/world/README.md).
