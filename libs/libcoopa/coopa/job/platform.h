/**
 * @file platform.h
 * @brief Platform detection macros and constants for the job system.
 *
 * Provides compile-time constants for cache-line sizing and platform-specific
 * optimizations. Automatically detects x86-64 vs ARM architectures.
 */

#ifndef COOPA_JOB_PLATFORM_H
#define COOPA_JOB_PLATFORM_H

#include <cstddef>
#include <cstdint>

namespace coopa {
namespace job {

/**
 * @brief Detects the hardware cache line size at compile time.
 *
 * Uses C++17 std::hardware_destructive_interference_size when available,
 * otherwise falls back to architecture-specific defaults:
 * - 128 bytes on Apple Silicon (ARM64 with __APPLE__)
 * - 64 bytes on all other platforms (x86-64, generic ARM, etc.)
 */
#if defined(__aarch64__) && defined(__APPLE__)
    inline constexpr std::size_t k_cache_line_size = 128;
#else
    inline constexpr std::size_t k_cache_line_size = 64;
#endif

/// @brief Maximum number of distinct job types supported by the flat type-count array.
inline constexpr uint32_t k_max_job_types = 64;

/// @brief Default maximum number of jobs that can be in-flight simultaneously.
inline constexpr uint32_t k_default_job_pool_capacity = 4096;

/// @brief Maximum number of dependencies a single job can declare (inline storage).
inline constexpr uint8_t k_max_inline_dependencies = 4;

/// @brief Number of spin iterations a worker performs before falling back to CV sleep.
inline constexpr uint32_t k_worker_spin_count = 256;

} // namespace job
} // namespace coopa

#endif // COOPA_JOB_PLATFORM_H
