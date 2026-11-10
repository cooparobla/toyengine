/**
 * @file model_ubo.h
 * @brief A 128-byte per-object model + normal matrix push-constant block.
 *
 * Uses push constants (not a UBO) for the per-object model data — push constants
 * avoid descriptor set updates for each draw call and fit within the guaranteed
 * 128-byte minimum push constant budget (two mat4s = 128 bytes exactly).
 */

#ifndef GFXCOOPA_ENGINE_DATA_MODEL_UBO_H
#define GFXCOOPA_ENGINE_DATA_MODEL_UBO_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace coopa {
namespace gfx {
namespace engine {
namespace data {

/**
 * @struct ModelPushConstants
 * @brief Per-object data uploaded as push constants.
 *
 * Exactly 128 bytes — fits the Vulkan guaranteed minimum push constant budget.
 *
 * - model:         Object-to-world transform matrix
 * - normal_matrix: Transpose of the inverse of the model matrix,
 *                  for correct normal transformation in non-uniform scaled objects
 */
struct ModelPushConstants {
    glm::mat4 model         = glm::mat4(1.0f); /**< Model (object-to-world) matrix. */
    glm::mat4 normal_matrix = glm::mat4(1.0f); /**< transpose(inverse(model)) for normals. */

    /**
     * @brief Sets model from a world matrix and recomputes the normal matrix.
     * @param world_matrix The object-to-world transform matrix.
     */
    void set_model(const glm::mat4& world_matrix) {
        model         = world_matrix;
        normal_matrix = glm::transpose(glm::inverse(world_matrix));
    }
};

static_assert(sizeof(ModelPushConstants) == 128,
    "ModelPushConstants must be exactly 128 bytes (two mat4s).");

} // namespace data
} // namespace engine
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_ENGINE_MODEL_UBO_H
