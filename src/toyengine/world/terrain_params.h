/**
 * @file terrain_params.h
 * @brief How a tile terrain's world is cut into tiles and chunks.
 *
 * Passed by value everywhere -- the mesher, the streamer and the map bake all need it, and it is
 * small, immutable and copyable on purpose so a meshing job can hold its own copy. Authored on
 * the `Terrain` component in `Tiles` mode (see terrain_component.h).
 */

#ifndef TOYENGINE_WORLD_TERRAIN_PARAMS_H
#define TOYENGINE_WORLD_TERRAIN_PARAMS_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include <glm/glm.hpp>

#include <toyengine/world/tile_types.h>   // ChunkCoord, which chunk_at() answers with

namespace toy {
namespace world {

/**
 * @struct ShoreProfile
 * @brief The slope cones `grade_tile_shore()` (terrain_shore.h) grades a Tiles shoreline with,
 *        in whole tile steps and tiles.
 *
 * A cone walks out from the waterline carrying the height the shore grades to. Each step costs
 * `(the ground's own rise between the two tiles) + a recovery slope`, clamped into
 * `[0, *_taper_slope_steps]`, and it stops at a tile the moment it MEETS the baked ground.
 *
 * Carrying the ground's own rise is what keeps a cone from excavating. A cone is a ceiling
 * (`min(baked, cone)`) or a floor, so one blind to the ground digs a flat shelf out of any slope
 * steeper than itself and leaves the whole depth of that excavation standing as a face where it
 * stops -- it MANUFACTURES a wall rather than failing to remove one. With the term, **ground no
 * steeper than `*_taper_slope_steps` is never touched at all**: a cone only falls into debt where
 * the terrain has a riser steeper than that, and repays it at the recovery slope, which runs from
 * `*_slope_steps` at the surface to `*_taper_slope_steps` once the cut is `cut_recovery_steps`
 * deep (`*_max_steps` deep for the fill and the bed, which have no separate ramp).
 *
 * Meeting the ground is also the one place a cone can stop without leaving a face, since
 * `min(baked, cone)` is exactly `baked` there. So every riser in the graded surface is either one
 * the terrain already had or at most `*_taper_slope_steps` -- except across a `*_max_steps`
 * refusal, where the cone declined to attack a bank taller than its budget and left it alone.
 * That refusal is what keeps a sea cliff a cliff.
 *
 * Slopes are steps per tile, so their angle depends on `height_step / tile_size`. The angles
 * quoted below are for a world where those are equal, which makes a slope a plain tangent.
 */
struct ShoreProfile {
    /** @brief Grades a Tiles shoreline at all. Off leaves settle_water_rounding()'s bare terrace. */
    bool enabled = false;

    /** @brief Dry side, ceiling: the gentlest grade, how a graded bank feathers back into flat
     *         ground once the cone has paid back its cut. 0.35 steps/tile ~= 19 deg. */
    float cut_slope_steps = 0.35f;
    /**
     * @brief The most the cut may move any tile below its baked height -- its budget, and so the
     *        tallest bank it attacks at all; past this a face is a cliff and is left standing.
     *
     * **Raising this to grade tall banks does not work, and the failure is in the architecture
     * rather than in the number.** A cone that stops anywhere other than at the ground leaves a
     * face as tall as whatever it had already cut, so the budget is simultaneously the tallest
     * bank attacked and the tallest face manufactured. Measured on the 6.29 M-tile globe, worst
     * face the grading adds: 6 steps at this value, 24 at `24`, and **74** with the refusal
     * removed entirely (the cone then runs to `cone_reach_tiles` and truncates there instead).
     * Breaking a tall riser into a walkable drop needs a mass-conserving relaxation over the
     * baked heights, not a wider cone.
     */
    float cut_max_steps = 6.0f;
    /**
     * @brief How deep a cut has to be before the cone recovers at the full taper slope, in steps.
     *
     * Separate from `cut_max_steps` on purpose. The recovery slope ramps from `cut_slope_steps` at
     * the surface to `cut_taper_slope_steps` at this depth, so this alone sets how a bank of a
     * given height feathers back into flat ground; the budget only sets which banks are attacked.
     * Folding the two together means raising the budget to reach a tall bank silently flattens
     * every short one as well, because each would then sit a smaller fraction of the way up the
     * ramp. Also the Dijkstra's monotonicity constant: the taper is capped at
     * `cut_slope_steps + this / sqrt(2)`.
     */
    float cut_recovery_steps = 6.0f;
    /** @brief Dry side: the steepest riser the graded surface may contain, and the slope the cone
     *         climbs back toward the ground at once its budget is spent. 2.0 ~= 63 deg. */
    float cut_taper_slope_steps = 2.0f;

    /** @brief Dry side, floor: grades the one-tile lip settle_water_rounding() leaves. ~22 deg. */
    float fill_slope_steps = 0.40f;
    /**
     * @brief The most a fill may raise a tile above its baked height -- so also the most material
     *        it may add, and the tallest drop below a bank it can grade out.
     *
     * This is what answers a PERCHED water body: a river or lake standing above the ground
     * outside it. Its waterline tiles are pinned at the water's terrace by the water invariant,
     * so neither the cut nor the mass-conserving talus relaxation may take material from them --
     * the only way to soften that outer bank is to raise the ground below it, which is this.
     * At 4 it could not reach: measured on the 6.29 M-tile globe, the max dry riser two tiles
     * from water stayed at **24 steps** however the talus was tuned (24 at repose 3.0, still 24
     * at 1.5 -- the giveaway that the repose angle was not what bound it), and fell to **6** --
     * matching every other distance -- once this could span the drop.
     *
     * The cone follows the ground (`follow_ground`), so it still cannot buttress out into thin
     * air over falling ground; the old cap was belt-and-braces from before the cone carried that
     * term.
     */
    float fill_max_steps = 24.0f;
    /** @brief Dry side, floor: 2.0 ~= 63 deg. */
    float fill_taper_slope_steps = 2.0f;

    /** @brief Wet side, floor: shelves a deep bed up toward its margin. ~14 deg. */
    float bed_slope_steps = 0.25f;
    /** @brief The most a bed may be raised toward its water's terrace. */
    float bed_max_steps = 6.0f;
    /**
     * @brief Least depth a bed is left at under its own water's terrace, in steps.
     *
     * One step is enough for water that sits on a single terrace. Where a river STEPS DOWN,
     * though, the chunk mesher hands a corner shared by two flooded tiles the MEAN of their
     * surface heights (`corner_z()`, terrain_chunk.cpp -- deliberately, so two chunks agree on a
     * shared border), which puts the surface about half the terrace drop below the upstream
     * terrace. A bed only one step down then stands proud of its own water and the surface cuts
     * visibly through the bank at every step. So the real floor is this OR half the largest
     * terrace fall to a wet neighbour, whichever is deeper, and this is only the flat-water case.
     *
     * Not capped: a river falling many terraces between adjacent tiles is a waterfall, and this
     * will cut a correspondingly deep pool under it. That is untested against a real waterfall --
     * the measured worlds step rivers down a terrace at a time -- so cap it here if one ever
     * turns up excavated.
     */
    std::int32_t bed_min_depth_steps = 1;
    /** @brief Wet side, floor: 3.0 ~= 72 deg. Submerged, so it may be steeper than the dry pair. */
    float bed_taper_slope_steps = 3.0f;

    /**
     * @brief How far a cone may run before it gives up looking for the ground, in tiles.
     *
     * A pathology guard, not a tuning knob: the ground is the real terminator, and a cone reaches
     * this only where the terrain climbs at its own taper slope for this many tiles on end.
     */
    float cone_reach_tiles = 24.0f;

    /**
     * @brief Angle of repose for the talus relaxation, in steps per tile; 0 disables it.
     *
     * The cones grade a bank they can afford to finish and leave anything taller standing (see
     * `cut_max_steps`). This is what breaks what they leave: a mass-conserving relaxation that
     * repeatedly moves material from the high side of any over-steep pair of dry tiles to the low
     * side until no riser exceeds this slope. Conserving mass is the whole point -- a transfer
     * only ever REDUCES the riser it acts on, so unlike a cone it cannot manufacture a face
     * taller than the one it removed, at any setting.
     *
     * 1.5 ~= 56 deg. 3.0 was tried first and is too steep to read as a slope: on 1 m tiles it
     * quantises into a run of 5-6 step risers, which from the ground is a wall again however
     * correct the number is. Judge this one in the probe, not in the riser table -- both values
     * satisfy "no riser over the repose", and only one of them looks like a bank.
     */
    float talus_slope_steps = 1.5f;
    /**
     * @brief How far from water the talus relaxation reaches, in tiles.
     *
     * Deliberately a band rather than the whole world: the vertical faces worth breaking are the
     * ones the water rounding and the cones leave at a shoreline, and relaxing inland terrain
     * would reshape every mountain and plateau in the scene for no stated reason.
     */
    float talus_band_tiles = 24.0f;
    /**
     * @brief Iteration cap for the relaxation; it normally stops earlier, when nothing moves.
     *
     * A guard, not a tuning knob. Reaching it leaves risers steeper than the repose slope rather
     * than anything invalid, since every pass is individually mass-conserving.
     */
    std::int32_t talus_max_passes = 64;

    /** @brief Quantisation dither amplitude, in steps: softens a terrace edge into short lobes. */
    float jitter_steps = 1.0f;
    /** @brief The dither's lattice period, in tiles, measured on the terrain's real surface. */
    float jitter_tiles = 4.0f;
    std::uint32_t jitter_seed = 0x5EA5A11Du;

    /** @brief How far from water the sand band reaches, before its own dither. */
    float beach_tiles = 6.0f;
    float beach_jitter_tiles = 3.0f;
    /** @brief A dry tile sands over only within this many steps of the water nearest it. */
    std::int32_t beach_rise_steps = 2;

    /**
     * @brief How much the sand band's width wanders along a coast, as a fraction of it.
     *        0 (the default) is one uniform band everywhere; 0.6 runs it from 0.4x to 1.6x.
     *
     * Without this a shoreline is the same width of sand from end to end, which reads as a
     * drawn outline rather than as beaches: a real coast is broad bays separated by stretches
     * with barely any sand at all. The scale multiplies BOTH limits on the band -- how far from
     * the water it may reach and how far above the water it may climb (`beach_rise_steps`) --
     * because on anything but dead-flat ground it is the rise that bounds the width, and
     * widening the reach alone would do nothing on a sloping shore.
     *
     * The variation is a smooth field, not the lattice dither `jitter_steps` uses: a bay should
     * open and close over its length, where an abrupt change of width would cut a straight edge
     * across the sand.
     */
    float beach_variation = 0.0f;
    /**
     * @brief How long a stretch of coast shares a width, in tiles -- the variation's period.
     *
     * Short periods read as a ragged edge (which `beach_jitter_tiles` already provides); this
     * wants to be the scale of a bay, tens of tiles rather than a handful.
     */
    float beach_variation_tiles = 64.0f;
    /** @brief Seed for that field; change it to re-roll which coasts are sandy. */
    std::uint32_t beach_variation_seed = 0xB3ACE5u;
    /** @brief Retextures sand/bed kinds and the biome byte; false grades height only. */
    bool retexture = true;
    /**
     * @brief Restricts the sand band to ground this pass itself moved.
     *
     * The band above decides where sand goes from a tile's distance to water and how far it
     * stands above it. That is the only answer available for a hand-authored terrain, but it is
     * the WRONG one for a terrain baked from a map that already classified its own shores:
     * mapcoopa's `Biome::Beach` is drawn where the ground stays within `beach_height_m` of the
     * water, which varies with the slope and so ends where the land climbs away -- broad across
     * a shelf, absent along a cliff. Drawn on top of that, a fixed tile-count band does not add
     * variety, it installs a FLOOR: measured on a baked globe it lifted the quietest quarter of
     * the coastline from 0.1 to 1.2 tiles of sand per tile of waterline while leaving the
     * broadest tenth where it already was. Every bare headland came out with the same stripe.
     *
     * Set true and the band only sands tiles whose height this pass changed -- the cut, fill and
     * talus passes run after the map classified the ground, so a bank they cut down to the
     * waterline is the one case the classifier could not have seen. Everywhere else the map's
     * answer stands, and the biome byte keeps agreeing with it.
     *
     * **River banks are exempt, and have to be.** A map classifies a shore from the cells that
     * hold water, and a river runs along the EDGE between two cells rather than filling one, so
     * no cell beside it is ever a water cell and its banks fall outside the classification
     * entirely. The band is the only thing that sands them either way, so it still draws there.
     */
    bool beach_repair_only = false;
};

/**
 * @struct TerrainParams
 * @brief How the map's continuous world is cut into discrete tiles and chunks.
 *
 * Authored in scene YAML on the `Terrain` component (see terrain_component.h) and then passed
 * by value everywhere -- the sampler, the chunk mesher and the streamer all need it, and it is
 * small, immutable and copyable on purpose so a meshing job can hold its own copy.
 */
struct TerrainParams {
    /**
     * @brief Tiles per mapcoopa grid unit -- the resolution the map is sampled at.
     *
     * One grid unit is one Voronoi cell's nominal width, so this is "how many blocks wide a map
     * cell is". Raising it resolves coastlines and river channels more finely at a quadratic
     * cost in tiles; it does NOT add detail the map does not have, beyond the fractal
     * displacement `MapConfig::terrain_roughness` already carries.
     */
    std::int32_t tiles_per_grid_unit = 4;

    /**
     * @brief The world's size in tiles per side -- how many tiles the map is rasterised into,
     *        whatever its cell count. 0 derives it: `grid_size * tiles_per_grid_unit`.
     *
     * Set, it wins over `tiles_per_grid_unit`: the map is sampled at `world_tiles / grid_size`
     * tiles per cell (fractional is fine), so the world's size and the map's detail are chosen
     * separately. The world spans `world_tiles * tile_size` world units.
     */
    std::int32_t world_tiles = 0;

    /** @brief Tiles per side for a map of `grid_size` cells. */
    std::int32_t tiles_for_grid(std::int32_t grid_size) const {
        return world_tiles > 0 ? world_tiles : grid_size * tiles_per_grid_unit;
    }

    /** @brief Horizontal size of one tile, in world units. */
    float tile_size = 1.0f;

    /** @brief Vertical size of one height step, in world units; the quantisation of the terrain. */
    float height_step = 1.0f;

    /**
     * @brief How many heights a column may stand at WITHIN one step. 1 is a terraced world.
     *
     * At 1 a column's height is a whole number of `height_step`s, so at `height_scale: 256` over
     * a 1 m step there is a 1 m riser under every square metre. Above 1 the bake keeps the
     * remainder in `TileCell::ground_sub` and the mesher lifts the column's whole stack by it:
     * the top face stays flat and hard-edged, every stamped piece keeps its exact height, and
     * the only new geometry is the band that closes a drop shorter than a step. Snapped to a
     * power of two up to `k_sub_steps` by sub_steps_for(); 16 is 6.25 cm at a 1 m step.
     *
     * It is NOT a finer `height_step`, which is the whole point: the step stays the step, so
     * every threshold denominated in steps -- soil depth, the shore budgets, a climbable riser,
     * the coarse tier's merges -- keeps the length and the cost it has.
     */
    std::int32_t height_fine = 1;

    /**
     * @brief Whether a sub-step difference TILTS the two tops to meet, instead of walling it.
     *
     * Off -- the default -- every tile top stays dead flat and a drop shorter than a step is
     * closed by one band stretched to exactly that drop. That is the voxel read: a tile is a
     * flat-topped block, just no longer one whose height is a whole number of steps.
     *
     * On, a sub-step difference is absorbed by sloping the tops into each other and no band is
     * emitted at all, which costs fewer triangles than terracing does. It reads as smooth
     * ground rather than fine blocks, and because the tiles along a contour of a smooth
     * elevation field quantise alike, the bands it avoids are the ones that would otherwise
     * line up into visible ribs. Both expressions read the SAME stored heights, so switching is
     * a re-mesh and never a re-bake.
     */
    bool height_fine_tilt = false;

    /** @brief World height of the full `[0, 1]` elevation range -- the vertical scale of the world. */
    float height_scale = 16.0f;

    /** @brief Tiles per chunk edge; a chunk is `chunk_size * chunk_size` columns. */
    std::int32_t chunk_size = 16;

    /**
     * @brief Chunks of terrain kept loaded in every direction from the camera's chunk.
     *
     * The live region is a square of side `2 * view_radius + 1`. It sets how much geometry is
     * resident and meshed; what is DRAWN is narrower -- toy::render::ToyRenderPipeline
     * frustum-culls every chunk against the camera and each shadow view separately.
     */
    std::int32_t view_radius = 3;

    /** @brief Steps of `side_kind` under a top face before a wall switches to bare Stone. */
    std::int32_t soil_depth_steps = 3;

    /**
     * @brief How far the water surface sits under the top of the terrace it fills, in steps.
     *
     * Water fills a column to a whole terrace (TileRecord::water_level()), and its banks stand
     * at least that high; drawn exactly at the terrace top, the surface would lie flush with
     * every bank top and fight it. Half a step puts the shoreline halfway down the bank's wall,
     * where whatever shape the tile set gives that wall -- rounded, square -- is the shoreline.
     */
    float water_inset = 0.5f;

    /**
     * @brief The same for rivers, in steps. A river runs in a channel one tile wide, its banks
     *        the terrace it fills to; at the sea's half step it reads as sunk in a trench, so it
     *        sits higher, just under the bank tops.
     */
    float river_inset = 0.2f;

    /**
     * @brief How tall a river's waves stand next to the still water's, 0..1.
     *
     * A river's surface is its current, not the sea's swell -- but a river and the lake it runs
     * into SHARE the vertices along their seam (build_chunk_water()), and the vertex stage
     * displaces a vertex from its own body's wave params. A body-wide difference therefore pulls
     * every shared vertex two ways until the two surfaces part and the bed shows through the gap.
     * So the difference is carried per corner instead (ChunkWaterData::wave_scales), meaned over
     * the tiles sharing each corner exactly as its height and depth are: both meshes read the
     * same value at the seam, and the waves ease across it over a tile.
     */
    float river_wave_scale = 0.25f;

    /**
     * @brief Tiles shoreline grading (terrain_shore.h's `grade_tile_shore()`), off by default.
     *
     * The terraced counterpart of the smooth-field keys below. `water_inset` / `river_inset` and
     * the skirt's lip drop only sink the WATER into whatever bank the tiles happen to form; they
     * cannot change the bank's own shape, so a steep coast or a cut river rim stays the vertical
     * riser `settle_water_rounding()` left. This is what grades that riser.
     */
    ShoreProfile shore;

    // --- Heightmap shoreline (terrain_water.h's grade_water_shore(), terrain_water_mesh.cpp) ---
    // The smooth-field counterpart of `shore` above, and unused in Tiles mode: a continuous field
    // has no terrace to grade, since its shoreline is simply where the ground crosses the water's
    // level, so the terrain's own slope is already the beach.

    /** @brief Metres a wet tile's bed is kept under its own water level (ShoreGrade::clearance). */
    float shore_clearance = 0.03f;

    /** @brief Extra bed depth reached `shore_grade_band` tiles in from the shoreline; 0 keeps the generated bed. */
    float shore_grade_depth = 0.0f;

    /** @brief Tiles the extra depth in `shore_grade_depth` ramps over. */
    float shore_grade_band = 4.0f;

    /**
     * @brief How far past the waterline the water surface runs, in METRES, buried under the bank.
     *
     * The smooth mesher's skirt (terrain_water_mesh.cpp): past the wet tiles themselves, the
     * surface keeps going for this much further, held flat at the water's level everywhere its
     * own ground is still underwater and tucked a few centimetres under it only where that ground
     * has risen to meet the surface. A metre or two is enough on a steep bank; a gentle beach
     * wants more so the polygon edge never surfaces in the open.
     *
     * Converted to a tile-count radius at the terrain's own `tile_extent()` where it is consumed
     * (`HeightmapTerrainSystem::refresh_chunk_water_()`), so the authored reach stays the same
     * physical width whatever `height_sub` or the heightmap's own `tile_size` happen to be.
     *
     * A raw tile count here instead would scale the apron's real-world reach -- and the size of
     * the hard, tile-quantised edge where it meets dry land -- by `tile_size`, so the same
     * authored number would mean a different width in any terrain whose tiles are not 1 m
     * (`unified_terrain_demo`'s default `height_sub: 2` among them).
     */
    float water_skirt_tiles = 4.0f;

    /**
     * @brief Whether this terrain permits rounding where two tile kinds meet at a corner.
     *
     * Rounding keys off the tile grid as it lies in the world plane, so a terrain whose
     * geometry is bent off that plane (world/chunk_shaper.h) has no grid left to round against
     * and keeps its corners square. Terrain-wide, and a veto only: a tile set that keeps kind
     * boundaries square (a blocky set) rounds nothing either way -- see the per-set
     * `round_kind_corners` key and TileMeshLibrary::rounds_kind_corners().
     */
    bool allow_kind_corner_rounding = true;

    /**
     * @brief Surface shader for this terrain's chunks; empty means the stock `terrain_styled`.
     *
     * A terrain whose geometry is bent off the world plane (world/chunk_shaper.h) needs a build
     * of the terrain shader that takes "up" from the per-vertex `uv.y` the shaping wrote rather
     * than from world +Z, so whatever installed the shaper names the matching shader here. The
     * stock shader carries none of that code.
     */
    std::string surface_shader;

    /**
     * @brief Tiles along one edge of a shaped world's face; a multiple of `chunk_size`.
     *
     * A world whose geometry leaves the world plane (world/chunk_shaper.h) is not one square of
     * `world_tiles`: it is a set of equal faces, stored side by side as one wide grid, and this
     * is one face's width. A shaper is handed nothing but `const TerrainParams&`, so its grid
     * dimension has to live here. Inert on a flat terrain, which uses `world_tiles`.
     */
    std::int32_t face_tiles = 128;
    /**
     * @brief Most tiles one merged top or wall run may span; 0 is unlimited.
     *
     * **A world whose surface is not the world plane cannot merge; author 1.** On flat ground a
     * merged rectangle is exactly coplanar with the per-tile geometry it borders, so the only
     * thing that differs is the subdivision -- a T-junction, whose cracks are sub-pixel. Bent off
     * the plane (world/chunk_shaper.h) a merged quad becomes a CHORD, and its edge no longer lies
     * on the surface its neighbours do: at four tiles over a radius of 122 units it deviates
     * ~1.7 cm from them, which is a visible seam on a 1 m tile at ground level. Lifting the quad
     * by its sagitta does not escape that -- it only turns the gap into a ledge of the same size,
     * which reads just as badly at a grazing angle. The width can be solved for against a
     * deviation tolerance, but no tolerance loose enough to be worth merging is tight enough to
     * be invisible, so the deviation itself is the thing to avoid.
     *
     * Measured before this was settled: deriving a width from curvature bought ~5% of the shadow
     * triangles and ~11% of load time near the surface, and nothing at all when zoomed out (no
     * chunk draws LOD 0 there). `lods:` field tiers are where a curved world's cost actually goes
     * -- see ChunkLod::field_stride.
     */
    std::int32_t max_merge = 0;

    // --- Level of detail (terrain_chunk.cpp builds these into every chunk mesh) ---

    /**
     * @struct ChunkLod
     * @brief One coarser level of a chunk's mesh: how much of it survives, and when it is drawn.
     */
    struct ChunkLod {
        float ratio       = 0.0f;  /**< Triangles kept, as a fraction of the full chunk. 0 ends the table. */
        float screen_size = 0.0f;  /**< Drawn while the chunk covers less of the screen's height than this. */
        /**
         * @brief Library style index to re-mesh this level in, instead of simplifying LOD 0.
         *
         * -1 (the default) simplifies LOD 0 as `ratio` says. A style level is re-meshed from the
         * chunk's own height field in that style and welded independently -- it does not share
         * LOD 0's vertices, so `ratio` is ignored for it. Resolved from a `tile_set:` entry under
         * `lods:` in scene YAML (register.cpp); never set by `ratio`-only levels.
         */
        std::int8_t style         = -1;
        /**
         * @brief Tiles of the chunk's rim kept at full detail -- in the authored (non-style) tile
         *        set for a `style` level, at one cell per tile for a `field` level.
         */
        std::int8_t native_border = 0;
        /**
         * @brief Tiles per cell to re-mesh this level as a quantised heightfield, instead of
         *        simplifying LOD 0; 0 (the default) is not a field level.
         *
         * One flat quad per `field_stride` x `field_stride` block of tiles at that block's
         * tallest column, with vertical walls wherever two blocks differ -- no tile pieces, no
         * lip, no corner rounding. Three properties `ratio` and `style` levels lack: its cost is
         * a function of `chunk_size / field_stride` alone, it can never be rejected the way a
         * `ratio` level whose pinned seam vertex collapses is, and it is meshed in face space
         * from the chunk's own columns, so **it is the one extra level a shaped world can
         * carry** -- a ChunkShaper bends it like any other geometry.
         *
         * Resolved from a `field:` entry under `lods:` in scene YAML (register.cpp); `ratio` is
         * ignored for it.
         */
        std::int8_t field_stride = 0;
        /**
         * @brief Shade this level as a smooth surface instead of hard-edged blocks.
         *
         * A `field` level is flat quads meeting vertical walls, so every edge between them is a
         * hard shading crease -- correct up close, but at the range these levels draw it reads as
         * a staircase where the real terrain is a rounded slope. With this set the level's
         * normals are averaged across the faces meeting at each position (smooth_field_normals(),
         * terrain_chunk.cpp) before it is welded: the GEOMETRY is untouched, so the level still
         * costs the same triangles and keeps the tallest-column seam rule that stops chunks
         * cracking -- only the shading stops breaking at every step.
         *
         * Off by default, which is the right choice for the FIRST coarse level: it is seen
         * closest, where the blocks genuinely are the terrain's shape. Resolved from a `smooth:`
         * entry under `lods:` in scene YAML (register.cpp).
         */
        bool smooth = false;
    };

    /** @brief Most levels a chunk mesh carries, past LOD 0. */
    static constexpr std::size_t k_max_chunk_lods = 4;

    /**
     * @brief The coarser levels every chunk mesh is built with, coarsest last; a zero `ratio`
     *        ends the table (all zero: no LOD, chunks always draw at full density).
     *
     * The defaults coarsen only what is genuinely small on screen: a chunk covering more than
     * 14% of the screen's height is untouched, so a camera on the ground sees exactly the mesh
     * it always did, while a world held whole in frame -- where one tile is a couple of pixels
     * and the terrain runs to four triangles a pixel -- draws a twentieth of them. A streamed
     * flat terrain only reaches these sizes with a large `view_radius`, which is the case that
     * wants them.
     *
     * Chunk boundaries are locked through every level (Mesh::simplify_levels()), so neighbours
     * at different levels meet on the same vertices and never crack.
     */
    std::array<ChunkLod, k_max_chunk_lods> chunk_lods{{{0.40f, 0.14f}, {0.15f, 0.07f}, {0.07f, 0.03f}, {}}};

    /** @brief World-space size of one chunk edge. */
    float chunk_world_size() const {
        return static_cast<float>(chunk_size) * tile_size;
    }

    /** @brief The per-axis scale of one tile cell, for TileMeshLibrary::append_styled(). */
    glm::vec3 cell_scale() const {
        return glm::vec3(tile_size, tile_size, height_step);
    }

    /** @brief The chunk containing a world position (ignoring z). */
    ChunkCoord chunk_at(const glm::vec3& world_position) const {
        const float size = chunk_world_size();
        return ChunkCoord{static_cast<std::int32_t>(std::floor(world_position.x / size)),
                          static_cast<std::int32_t>(std::floor(world_position.y / size))};
    }
};

} // namespace world
} // namespace toy

#endif // TOYENGINE_WORLD_TERRAIN_PARAMS_H
