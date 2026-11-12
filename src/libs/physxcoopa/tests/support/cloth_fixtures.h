#pragma once

/**
 * @file cloth_fixtures.h
 * @brief Shared cloth builders: the standard 4 x 4 m test sheet and a stationary ClothCollider
 *        around a shape (for driving cloth::project_particle() directly).
 */

#include <physxcoopa/cloth/cloth_builder.h>
#include <physxcoopa/cloth/cloth_solver.h>
#include <physxcoopa/collision/shape.h>

namespace physxtest {

using namespace coopa::physx;

/** @brief Builds a 4x4 m sheet centred at `center`, with the given resolution and params. */
inline cloth::Cloth make_test_sheet(uint32_t res, const glm::vec3& center, const cloth::ClothParams& params = {}) {
    cloth::GridClothDesc desc;
    desc.columns = res;
    desc.rows = res;
    desc.width = 4.0f;
    desc.height = 4.0f;
    desc.center = center;
    desc.total_mass = 1.0f;
    desc.params = params;
    return cloth::make_grid_cloth(desc);
}

/** @brief A stationary collider at the origin around `shape` (which must outlive it). */
inline cloth::ClothCollider still_collider(collision::Shape& shape) {
    shape.enabled = true;
    cloth::ClothCollider collider;
    collider.shape = &shape;
    collider.position = glm::vec3(0.0f);
    collider.bounds = collision::world_bounds(shape, collider.position, collider.orientation);
    return collider;
}

} // namespace physxtest
