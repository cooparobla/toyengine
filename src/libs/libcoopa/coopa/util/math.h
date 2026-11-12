/**
 * @file math.h
 * @brief Utility structures and functions for matrix transformations and general mathematics.
 */

#ifndef COOPA_UTIL_MATH_H
#define COOPA_UTIL_MATH_H

#include <cstdlib>
#include <iostream>
#include <iomanip>

#include <glm/glm.hpp>

/**
 * @struct Mat4
 * @brief A 4x4 matrix implementation optimized for column-major printing and local multiplication hazards.
 *
 * Provided for consumers; libcoopa itself uses glm types throughout.
 */
struct Mat4 {
    float m[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}; /**< The 4x4 float array representing the matrix cells. */

    /**
     * @brief Creates an identity matrix.
     * @return Mat4 representing identity matrix.
     */
    static Mat4 identity() { return Mat4{}; }

    /**
     * @brief Creates a translation matrix.
     * @param x X translation distance.
     * @param y Y translation distance.
     * @param z Z translation distance.
     * @return Mat4 translation matrix.
     */
    static Mat4 translation(float x, float y, float z) {
        Mat4 mat = identity();
        mat.m[3][0] = x;
        mat.m[3][1] = y;
        mat.m[3][2] = z;
        return mat;
    }

    /**
     * @brief Performs matrix multiplication (this * right).
     * @param right The matrix to multiply with.
     * @return Resulting Mat4 product.
     */
    Mat4 operator*(const Mat4& right) const {
        Mat4 result;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                result.m[i][j] = 
                    m[i][0] * right.m[0][j] +
                    m[i][1] * right.m[1][j] +
                    m[i][2] * right.m[2][j] +
                    m[i][3] * right.m[3][j];
            }
        }
        return result;
    }

    /**
     * @brief Converts the Mat4 into a GLM 4x4 column-major matrix representation.
     * @return glm::mat4 equivalent matrix.
     */
    glm::mat4 get_mat() const {
        return glm::mat4(
            m[0][0], m[1][0], m[2][0], m[3][0], // Column 0 (X-axis)
            m[0][1], m[1][1], m[2][1], m[3][1], // Column 1 (Y-axis)
            m[0][2], m[1][2], m[2][2], m[3][2], // Column 2 (Z-axis)
            m[0][3], m[1][3], m[2][3], m[3][3]  // Column 3 (Translation/W)
        ); 
    }

    /**
     * @brief Creates a Mat4 matrix from a GLM 4x4 matrix.
     * @param glm_matrix Source GLM matrix.
     * @return Converted Mat4 matrix.
     */
    static inline Mat4 from_mat(glm::mat4& glm_matrix) {
        Mat4 m;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                m.m[i][j] = glm_matrix[j][i]; // Note the transpose due to column-major order
            }
        }
        return m;
    }
    
    /**
     * @brief Utility helper that prints the matrix in a human-readable column-major format.
     */
    void log() const {
        std::cout << std::fixed << std::setprecision(2);
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                std::cout << m[j][i] << "\t"; // Column-major for transform printing
            }
            std::cout << std::endl;
        }
    }
};

/**
 * @class MathUtil
 * @brief Static helper functions for basic math operations.
 */
class MathUtil
{
public:
    /**
     * @brief Returns the maximum of two floating-point numbers.
     * @param i1 First float.
     * @param i2 Second float.
     * @return Max float.
     */
    static float get_max(float i1, float i2)
     {
         return (i1 > i2) ? i1 : i2; 
     }

    /**
     * @brief Returns the minimum of two floating-point numbers.
     * @param i1 First float.
     * @param i2 Second float.
     * @return Min float.
     */
    static float get_min(float i1, float i2)
     {
         return (i1 < i2) ? i1 : i2; 
     }

    /**
     * @brief Linearly interpolates between two floats.
     * @param a Start value.
     * @param b End value.
     * @param t Interpolation factor (0 to 1).
     * @return Interpolated float value.
     */
    static float lerp(float a, float b, float t)
     {
         return a + t * (b - a);
     }

private:
    MathUtil() = delete; /**< Disallow instantiation. */
};

#endif // COOPA_UTIL_MATH_H
