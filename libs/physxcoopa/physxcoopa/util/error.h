/**
 * @file error.h
 * @brief Common error-reporting helper for physxcoopa.
 */

#ifndef PHYSXCOOPA_UTIL_ERROR_H
#define PHYSXCOOPA_UTIL_ERROR_H

#include <stdexcept>
#include <string>

namespace coopa {
namespace physx {
namespace util {

/**
 * @brief Throws a std::runtime_error tagged with the "[physxcoopa]" prefix used
 *        consistently across the module, so every physics-originated exception is
 *        identifiable at a glance in a mixed-engine log or crash report.
 *
 * @param message Human-readable description of the failure.
 * @throws std::runtime_error Always.
 */
[[noreturn]] inline void throw_error(const std::string& message) {
    throw std::runtime_error("[physxcoopa] " + message);
}

} // namespace util
} // namespace physx
} // namespace coopa

#endif // PHYSXCOOPA_UTIL_ERROR_H
