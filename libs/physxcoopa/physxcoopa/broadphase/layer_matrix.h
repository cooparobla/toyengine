/**
 * @file layer_matrix.h
 * @brief 32-bit collision-layer matrix: which pairs of layers are allowed to generate contacts.
 */

#ifndef PHYSXCOOPA_BROADPHASE_LAYER_MATRIX_H
#define PHYSXCOOPA_BROADPHASE_LAYER_MATRIX_H

#include <cstdint>

namespace coopa {
namespace physx {
namespace broadphase {

/**
 * @class LayerMatrix
 * @brief Symmetric 32x32 collision matrix, tested before any narrowphase work.
 *
 * Every layer collides with every other layer by default.
 */
class LayerMatrix {
public:
    LayerMatrix() {
        for (uint32_t i = 0; i < 32; ++i) mask_[i] = 0xFFFFFFFFu;
    }

    /** @brief True if layers `a` and `b` are permitted to collide. */
    bool should_collide(uint32_t a, uint32_t b) const {
        return (mask_[a & 31] & (1u << (b & 31))) != 0;
    }

    /** @brief Enables or disables collision between layers `a` and `b` (symmetric). */
    void set_layer_collision(uint32_t a, uint32_t b, bool collide) {
        a &= 31;
        b &= 31;
        if (collide) {
            mask_[a] |= (1u << b);
            mask_[b] |= (1u << a);
        } else {
            mask_[a] &= ~(1u << b);
            mask_[b] &= ~(1u << a);
        }
    }

private:
    uint32_t mask_[32];
};

} // namespace broadphase
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_BROADPHASE_LAYER_MATRIX_H
