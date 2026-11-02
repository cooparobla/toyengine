/**
 * @file damage_field.h
 * @brief Localized damage accumulation over an object's local bounds -- pure, so repeated hits
 *        carve progressive LOCAL damage instead of draining one global HP bar.
 *
 * A dense scalar grid, allocated lazily by its owner (`Damageable`) on first hit, so an
 * undamaged object pays nothing. A 16-cells-on-the-longest-axis field is about 16 KB.
 *
 * No scene types, no mesh types: `deposit()` takes a local-space point/direction/edge and a
 * `damage::ResolvedDamage` (physxcoopa/damage/damage_resolve.h), and the field just accumulates
 * saturation -- what that saturation CAUSES (a slice, a fracture) is `DestructionSystem`'s job.
 */

#ifndef TOYENGINE_DESTRUCT_DAMAGE_FIELD_H
#define TOYENGINE_DESTRUCT_DAMAGE_FIELD_H

#include <physxcoopa/damage/damage_resolve.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace toy {
namespace destruct {

/**
 * @class DamageField
 * @brief A local-space grid of 0..1 saturation cells.
 */
class DamageField {
public:
    /** @brief (Re)allocates the grid over `bounds_min`/`bounds_max`, zeroed, with the longest
     *         axis split into `longest_axis_cells` cells (the others sized to match, rounded
     *         up) -- so a tall thin object gets a tall thin grid, not a cube of mostly-unused
     *         cells. Discards any damage already accumulated. */
    void reset(const glm::vec3& bounds_min, const glm::vec3& bounds_max, int longest_axis_cells = 16);

    /**
     * @brief Deposits a resolved blow, shaped by its footprint: a round dot for an isotropic
     *        hit, or a capsule along `local_edge` for a slash (`dmg.along_edge` half-length,
     *        `dmg.radius` thickness). Saturating falls off linearly with distance from the
     *        footprint and never exceeds 1 per cell -- a spot already cut through stays cut.
     * @param local_dir Reserved for a future directional (not just radial) footprint; unused
     *                   today, but part of the call so a caller does not have to change later.
     * @return The field's new total() (mean saturation, 0..1).
     */
    float deposit(const glm::vec3& local_point, const glm::vec3& local_dir, const glm::vec3& local_edge,
                 const coopa::physx::damage::ResolvedDamage& dmg);

    /** @brief Saturation at the cell nearest `local_point`, 0..1; 0 if the field is unallocated
     *         or the point falls outside its bounds. */
    float at(const glm::vec3& local_point) const;

    /** @brief Mean saturation over every cell, 0..1. 0 for an unallocated field. */
    float total() const;

    /** @brief Cells at or past 1.0 -- fully cut through. */
    int saturated_cells() const;

    /**
     * @brief Best-fit plane through the saturated cells, by PCA (the thinnest principal axis is
     *        the normal) -- how a slash streak that has cut all the way through becomes a
     *        cleave plane that follows the damage, not an arbitrary angle.
     * @param out_flatness 0..1: 1 when the saturated cells are nearly coplanar (the smallest
     *                     principal variance is negligible next to the largest), low when they
     *                     are scattered. The caller should refuse a low-flatness plane.
     * @return False when fewer than 4 cells are saturated.
     */
    bool saturated_plane(glm::vec3& out_normal, float& out_distance, float& out_flatness) const;

    /** @brief Local-space AABB of the saturated cells (expanded to their full cell volume, not
     *         just their centres) -- the region a localized fracture would break off, leaving
     *         the rest of the mesh whole.
     * @return False when no cell is saturated.
     */
    bool saturated_bounds(glm::vec3& out_min, glm::vec3& out_max) const;

private:
    std::size_t index_(int x, int y, int z) const {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(dims_.y) +
               static_cast<std::size_t>(y)) * static_cast<std::size_t>(dims_.x) +
               static_cast<std::size_t>(x);
    }
    glm::vec3 cell_centre_(int x, int y, int z) const {
        return origin_ + cell_ * (glm::vec3(x, y, z) + 0.5f);
    }
    bool cell_of_(const glm::vec3& local_point, glm::ivec3& out) const;

    glm::vec3  origin_{0.0f};   ///< Local-space position of cell (0,0,0)'s negative corner.
    float      cell_ = 0.1f;
    glm::ivec3 dims_{0};
    std::vector<float> cells_;
};

} // namespace destruct
} // namespace toy

#endif // TOYENGINE_DESTRUCT_DAMAGE_FIELD_H
