/**
 * @file instance_select.h
 * @brief A set of selected painted instances, and the edits you can apply to one — as **pure
 *        functions**, like the rest of `editor/instance/`.
 *
 * The painter's counterpart to `TileSelection` (editor/terrain/tile_brush.h). A tile selection
 * is a mask over a fixed grid; instances have no grid, so a selection is a list of
 * `InstanceRef`s — which chunk, and which record in it.
 *
 * ### What a ref survives
 *
 * A ref is an **index**, so it stays valid across edits that rewrite records in place (turning
 * them, rescaling them) and goes stale the moment a chunk's records are added to or removed
 * from, because erasing compacts the vector and every later index shifts down. Acting on a
 * stale ref would silently edit the wrong instance, so the panel clears the selection whenever
 * the set changes under it and `validate()` drops anything left addressing nothing. Both are
 * cheap; neither is optional.
 */

#ifndef TOYEDITOR_INSTANCE_INSTANCE_SELECT_H
#define TOYEDITOR_INSTANCE_INSTANCE_SELECT_H

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include <toyengine/instance/instance_set.h>

namespace toy::editor {

/**
 * @class InstanceSelection
 * @brief The selected instances. See the file comment for what a ref survives.
 */
class InstanceSelection {
public:
    using Ref = toy::instance::InstanceRef;

    bool empty() const { return refs_.empty(); }
    std::size_t size() const { return refs_.size(); }
    const std::vector<Ref>& refs() const { return refs_; }
    void clear() { refs_.clear(); }

    bool contains(Ref r) const;
    /** @brief Adds `r` if it is not already in. */
    void add(Ref r);
    /** @brief Removes `r` if it is in. */
    void remove(Ref r);
    /** @brief Adds `r` if absent, removes it if present. */
    void toggle(Ref r);

    /** @brief Drops every ref that no longer addresses a record. */
    void validate(const toy::instance::InstanceSetData& data);

    /** @brief The chunks the selection touches, for an undo patch. */
    std::vector<toy::instance::ChunkCoord> chunks() const;

    /** @brief Mean world position of the selected instances; origin when empty. */
    glm::vec3 centroid(const toy::instance::InstanceSetData& data) const;

    /**
     * @brief Applies `edit` to every selected record, chunk by chunk.
     *
     * Goes through `InstanceSetData::set_records()` rather than handing out writable records,
     * so each touched chunk's revision moves exactly once and every consumer -- the render
     * cache, the collider streamer -- rebuilds it.
     *
     * @return How many records were edited.
     */
    std::size_t edit(toy::instance::InstanceSetData& data,
                     const std::function<void(toy::instance::InstanceRecord&)>& edit) const;

    /** @brief Erases every selected record. Leaves the selection empty (its refs are gone). */
    std::size_t erase(toy::instance::InstanceSetData& data);

private:
    std::vector<Ref> refs_;
};

// --- per-record edits -----------------------------------------------------------------------

/**
 * @brief The instance's heading: its local +X projected onto the world XY plane, radians.
 *
 * Defined off the facing rather than by decomposing the quaternion, because a record's rotation
 * is a tilt onto the surface composed with a spin and there is no clean Euler to read back out
 * of that. An instance lying flat on a vertical wall has no meaningful heading; it reports 0.
 */
float instance_yaw(const toy::instance::InstanceRecord& r);

/**
 * @brief Turns the instance `radians` about the WORLD up axis.
 *
 * Pre-multiplied, so an instance tilted onto a slope spins about the vertical the way you
 * expect rather than about its own leaning axis.
 */
void turn_instance(toy::instance::InstanceRecord& r, float radians);

/** @brief Turns the instance so `instance_yaw()` becomes `radians`. */
void set_instance_yaw(toy::instance::InstanceRecord& r, float radians);

/** @brief Multiplies the instance's uniform scale, clamped to (0.01, 100]. */
void scale_instance(toy::instance::InstanceRecord& r, float factor);

/** @brief Sets the instance's uniform scale, clamped to (0.01, 100]. */
void set_instance_scale(toy::instance::InstanceRecord& r, float scale);

/** @brief Moves the instance by `delta` metres. */
void offset_instance(toy::instance::InstanceRecord& r, const glm::vec3& delta);

/**
 * @brief Turns the instance about `pivot` (world space), position and rotation together.
 *
 * What rotating a GROUP means: the instances swing around their shared centre instead of each
 * spinning in place, which is the difference between turning a camp and scrambling it.
 */
void orbit_instance(toy::instance::InstanceRecord& r, const glm::vec3& pivot, float radians);

} // namespace toy::editor

#endif // TOYEDITOR_INSTANCE_INSTANCE_SELECT_H
