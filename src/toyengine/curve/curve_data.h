/**
 * @file curve_data.h
 * @brief `CurveData` -- an authored cubic Bezier path, and the sampling every consumer uses.
 *
 * Pure data and pure functions: no scene, no device, no asset. A `Curve` component stores one,
 * the editor's pen tool edits one, and a procedural graph reads one. Keeping evaluation here
 * rather than in any of the three means a point the editor draws is the same point the bake
 * instances on -- the thing that goes wrong when a tool and a consumer each do their own maths.
 *
 * The shape is Blender's: a list of control points, each with an incoming and outgoing handle in
 * the SAME space as the point, and a `closed` flag that wraps the last segment back to the first.
 */

#ifndef TOYENGINE_CURVE_CURVE_DATA_H
#define TOYENGINE_CURVE_CURVE_DATA_H

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace toy {
namespace curve {

/// @brief How a control point's two handles relate when one is dragged.
enum class HandleMode : std::uint8_t {
    Free,     ///< The two move independently: a corner.
    Aligned,  ///< Opposite directions, independent lengths: smooth through the point.
    Vector,   ///< Handles point at the neighbouring control points: straight segments.
    Auto,     ///< Both derived from the neighbours (Catmull-Rom-like); dragging one makes it Free.
};

/// @brief One control point: where it is, and how the curve leaves and arrives.
struct CurvePoint {
    glm::vec3  position{0.0f};
    /// @brief The handle the curve ARRIVES along, in the curve's own space (not a delta).
    glm::vec3  in_handle{0.0f};
    /// @brief The handle the curve LEAVES along, same space.
    glm::vec3  out_handle{0.0f};
    HandleMode mode = HandleMode::Aligned;
    /// @brief Rolls the frame about the tangent, in degrees. What a railing's posts lean by.
    float      tilt = 0.0f;
};

/**
 * @struct CurveData
 * @brief The authored curve: control points, whether it closes, and how finely it samples.
 */
struct CurveData {
    std::vector<CurvePoint> points;
    /// @brief The last point's out-handle feeds the first's in-handle.
    bool closed = false;
    /**
     * @brief Samples per segment when the curve is turned into a polyline.
     *
     * Per SEGMENT, not per curve, so adding a point does not coarsen everything before it.
     */
    int resolution = 12;

    bool empty() const { return points.size() < 2; }
    /// @brief Segments between control points: one fewer than the points, or one per point closed.
    std::size_t segment_count() const;
};

// --- Evaluation -------------------------------------------------------------------------------

/**
 * @brief The point at parameter `t` within segment `segment`.
 * @param t 0 at the segment's start control point, 1 at its end.
 */
glm::vec3 evaluate_segment(const CurveData& c, std::size_t segment, float t);

/// @brief The unit tangent at (`segment`, `t`); the segment's chord when the derivative vanishes.
glm::vec3 tangent_at(const CurveData& c, std::size_t segment, float t);

/**
 * @brief The whole curve flattened to a polyline, `resolution` samples per segment.
 *
 * The shared sampling: the viewport draws this, and a bake instances along it.
 */
std::vector<glm::vec3> polyline(const CurveData& c);

/// @brief Approximate arc length, measured on `polyline()` so it agrees with what is drawn.
float length(const CurveData& c);

/**
 * @brief `count` positions spread evenly BY ARC LENGTH along the curve, with their frames.
 *
 * Even spacing is what a railing or a fence wants, and it is not what even `t` gives: a long
 * segment and a short one carry the same number of samples in parameter space.
 *
 * `from`/`to` trim the run to a fraction of the curve's LENGTH, so a graph can take the top
 * third of a trunk for its foliage without a second curve. A trimmed run always spreads its
 * points across the whole trim, closed or not -- the wrap-without-doubling rule only applies to
 * an untrimmed closed curve, where the first and last point would otherwise coincide.
 *
 * @param count       Points wanted; fewer than 2 yields an empty result.
 * @param out_tangents Filled with the unit tangent at each point when non-null.
 * @param from        Where to start, as a fraction of total length (0..1).
 * @param to          Where to stop, same units. `to <= from` yields an empty result.
 */
std::vector<glm::vec3> points_along(const CurveData& c, int count,
                                    std::vector<glm::vec3>* out_tangents = nullptr,
                                    float from = 0.0f, float to = 1.0f);

/**
 * @brief Rotation-minimising frames along a run of tangents (parallel transport).
 *
 * `frame_from_tangent()` derives each frame independently from world up, so the ROLL about the
 * tangent jumps as the tangent turns -- and consecutive segments instanced on those frames come
 * out rolled differently. On a faceted mesh that shows as mismatched facets and segments that
 * alternate light and dark for no visible reason.
 *
 * This carries the first frame along the curve instead: each is the previous one turned by the
 * smallest rotation that takes the old tangent to the new. Roll then changes only as much as
 * the curve forces it to.
 *
 * @param tangents    Unit tangents, in order.
 * @param tilt_degrees Roll applied to every frame after transport.
 */
std::vector<glm::quat> parallel_frames(const std::vector<glm::vec3>& tangents,
                                       float tilt_degrees = 0.0f);

/// @brief A rotation whose +X is `tangent`, rolled by `tilt_degrees` about it. +Z stays up-ish.
glm::quat frame_from_tangent(const glm::vec3& tangent, float tilt_degrees);

// --- Authoring --------------------------------------------------------------------------------

/**
 * @brief Recomputes the handles of every point whose mode derives them (`Vector`, `Auto`).
 *
 * Called after any edit that moves a point, because an Auto point's handles depend on where its
 * NEIGHBOURS are -- so moving one point reshapes the two beside it.
 */
void refresh_handles(CurveData& c);

/**
 * @brief Appends a control point, giving it handles that continue the curve.
 * @return The new point's index.
 */
std::size_t append_point(CurveData& c, const glm::vec3& position);

/**
 * @brief Splits `segment` at `t`, inserting a control point that leaves the shape unchanged.
 *
 * De Casteljau: the two halves of a cubic through the split reproduce the original exactly, so
 * inserting a point is a pure refinement and never a nudge.
 *
 * @return The new point's index, or the point count when `segment` is out of range.
 */
std::size_t insert_point(CurveData& c, std::size_t segment, float t);

/// @brief Removes point `index`. @return Whether it existed.
bool remove_point(CurveData& c, std::size_t index);

/// @brief Moves point `index` and its handles by `delta`, refreshing derived neighbours.
void move_point(CurveData& c, std::size_t index, const glm::vec3& delta);

/**
 * @brief Sets point `index`'s handle mode directly, as the Pen tool's anchor-mode buttons do.
 *
 * `Free` leaves the handles exactly where they are -- converting to a corner unlinks them
 * without moving anything. `Vector`/`Auto` recompute immediately via `refresh_handles()`, the
 * same as a move does, so the new mode is visible without a further edit. `Aligned` mirrors
 * one handle's direction onto the other, keeping each its own length; when both already sit on
 * the point (an endpoint never dragged), the direction instead comes from the neighbours, the
 * same tangent `Auto` would compute.
 */
void set_handle_mode(CurveData& c, std::size_t index, HandleMode mode);

} // namespace curve
} // namespace toy

#endif // TOYENGINE_CURVE_CURVE_DATA_H
