/**
 * @file instance_brush.h
 * @brief The instance painter's tools, as **pure functions**: where a dab would put instances,
 *        what each instance ends up looking like, and the undo patch a stroke records.
 *
 * No scene, no GPU, no app -- the editor's Instance Paint mode
 * (editor/app/ui/instance_paint.cpp) drives these and tests call them directly, exactly as
 * `editor/terrain/` does for the terrain brushes. The surface is handed in: these functions
 * never raycast, so a test can paint onto a flat plane it made up.
 *
 * Brushes work in metres. A dab is a disc on the xy plane; `terrain_falloff()` (reused from
 * the terrain brush, so the two feel the same) is the per-candidate keep probability, which is
 * why a stroke thins toward its rim instead of ending in a hard circle of props.
 */

#ifndef TOYEDITOR_INSTANCE_INSTANCE_BRUSH_H
#define TOYEDITOR_INSTANCE_INSTANCE_BRUSH_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/instance/instance_painter.h>
#include <toyengine/instance/instance_set.h>

namespace toy::editor {

/** @brief What the brush does. Grid snapping is a modifier on these, not a tool of its own. */
enum class InstanceTool {
    Paint = 0,  ///< Scatters instances under the brush at `density`, thinning toward the rim.
    Erase,      ///< Removes instances under the brush.
    Place,      ///< One instance per click, exactly where the cursor is.
    Cluster,    ///< One click drops a clump: `cluster_count` instances within `cluster_spread`.
    /**
     * @brief Picks instances already placed, to turn, rescale, move or delete them.
     *
     * Click one, or drag a box over many (Shift adds, Ctrl removes). Unlike the other four it
     * places nothing: it is how you fix what the brush put down. See editor/instance/instance_select.h.
     */
    Select,
};

/** @brief Display name ("Paint", ...). */
const char* instance_tool_name(InstanceTool t);

/** @brief Every setting the brush has. One of these lives on the panel. */
struct InstanceBrushSettings {
    InstanceTool tool = InstanceTool::Paint;
    float radius = 4.0f;        ///< Metres.
    float density = 1.0f;       ///< Instances per square metre at full strength.
    float falloff = 0.6f;       ///< 0 = hard edge, 1 = thinning all the way to the centre.
    float spacing = 0.35f;      ///< Dab spacing along a drag, as a fraction of the radius.
    /**
     * @brief Metres an instance keeps clear of every existing one.
     *
     * Part of the density model, not a surface filter: without it a slow drag piles a hundred
     * trees on one spot, because every dab re-rolls the same disc.
     */
    float min_spacing = 0.5f;
    int   prototype = 0;        ///< Index into the painter's palette.
    /** @brief Erase only instances of `prototype`; off erases whatever is under the brush. */
    bool  erase_active_only = false;

    // --- Cluster ---
    glm::ivec2 cluster_count{3, 7};   ///< Instances per clump, inclusive range.
    float cluster_spread = 1.5f;      ///< Metres from the clicked point.
    float cluster_bias = 0.5f;        ///< 0 spread evenly .. 1 crowded at the centre.
    bool  cluster_mixed = false;      ///< Draw a prototype at random per instance.
    int   cluster_prototypes = 1;     ///< How many of the palette's first entries to mix.

    // --- Snap ---
    bool  snap = false;               ///< Snap placement to a grid and yaw to `yaw_step`.
    float grid_cell = 1.0f;           ///< Metres per cell.
    glm::vec2 grid_origin{0.0f};      ///< The grid's phase, metres.
    float yaw_step = 90.0f;           ///< Degrees; 0 leaves yaw alone.
    bool  one_per_cell = true;        ///< A cell that already holds an instance takes no more.
};

/** @brief A point on the surface a candidate landed on: what `make_record()` orients to. */
struct SurfaceSample {
    glm::vec3 position{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
};

/**
 * @brief The xy points a dab of `s` at `centre` would try to place an instance at.
 *
 * A jittered grid at `1 / sqrt(density)` spacing over the dab's disc -- the same construction
 * the foliage scatter uses (toyengine/particles/terrain_scatter.h), so density means the same
 * thing in both and neither clumps. Every random number is hashed from `(seed, cell)`, so the
 * same dab always produces the same candidates whatever order they are consumed in.
 *
 * Candidates are **not** filtered against the surface or against existing instances: the
 * caller raycasts each one and applies `min_spacing` / `one_per_cell` as it goes, because both
 * depend on what the earlier candidates of the same dab did.
 */
std::vector<glm::vec2> brush_candidates(const InstanceBrushSettings& s, glm::vec2 centre, std::uint32_t seed);

/** @brief `p` moved to the centre of its grid cell. */
glm::vec2 snap_to_grid(const InstanceBrushSettings& s, glm::vec2 p);

/**
 * @brief The xy points one clump at `centre` would place instances at.
 *
 * `cluster_count` of them (inclusive, hashed from `seed`) within `cluster_spread`, distributed
 * by `cluster_bias`: 0 spreads them evenly over the disc, 1 crowds them at the middle.
 */
std::vector<glm::vec2> cluster_candidates(const InstanceBrushSettings& s, glm::vec2 centre, std::uint32_t seed);

/**
 * @brief The prototype a cluster candidate takes, given `cluster_mixed` and the palette size.
 * @param palette_size How many prototypes the painter actually has.
 */
int cluster_prototype(const InstanceBrushSettings& s, int palette_size, std::uint32_t seed);

/**
 * @brief Builds the record for one instance: the prototype's variation ranges rolled from
 *        `seed`, oriented to `surface`, sunk by `sink`.
 *
 * Deterministic: the same (prototype, surface, settings, seed) always gives the same record,
 * which is what lets a test assert a stroke's output exactly.
 */
toy::instance::InstanceRecord make_record(const toy::instance::InstancePrototype& proto, int prototype_index,
                                          const SurfaceSample& surface, const InstanceBrushSettings& s,
                                          std::uint32_t seed);

/**
 * @struct InstancePatch
 * @brief The chunks a stroke touched, as they were before or after it -- what one undo step
 *        restores. Holds the data weakly: a painter removed since is skipped.
 *
 * Whole chunks rather than individual records, because a stroke's effect on a chunk is not a
 * list of insertions: `min_spacing` makes which candidates survive depend on the order they
 * were tried, and an erase removes records the patch would otherwise have to name by index.
 * A 4 m brush touches at most four 32 m chunks, so the snapshot is cheap.
 */
struct InstancePatch {
    std::weak_ptr<toy::instance::InstanceSetData> data;
    std::vector<std::pair<toy::instance::ChunkCoord, std::vector<toy::instance::InstanceRecord>>> chunks;

    std::size_t bytes() const;
};

/** @brief Copies `chunks` of `data` into a patch. */
InstancePatch capture_instance_patch(const std::shared_ptr<toy::instance::InstanceSetData>& data,
                                     const std::vector<toy::instance::ChunkCoord>& chunks);

/** @brief Writes a patch back. False when the data is gone. */
bool apply_instance_patch(const InstancePatch& patch);

} // namespace toy::editor

#endif // TOYEDITOR_INSTANCE_INSTANCE_BRUSH_H
