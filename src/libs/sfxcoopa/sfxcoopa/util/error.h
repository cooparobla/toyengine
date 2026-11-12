/**
 * @file error.h
 * @brief miniaudio result checking utilities for sfxcoopa.
 *
 * Mirrors gfxcoopa/util/error.h's shape (a *_check(result, context) function
 * throwing std::runtime_error, plus a *_CHECK macro capturing the call-site
 * expression as text), adapted to miniaudio's ma_result instead of VkResult.
 * Unlike Vulkan, miniaudio ships its own result -> string mapping
 * (ma_result_description), so there is no need to hand-roll a switch here.
 */

#ifndef SFXCOOPA_UTIL_ERROR_H
#define SFXCOOPA_UTIL_ERROR_H

#include <miniaudio/miniaudio.h>

#include <stdexcept>
#include <string>

namespace coopa {
namespace sfx {
namespace util {

/**
 * @brief Checks an ma_result and throws std::runtime_error on failure.
 *
 * @param result The ma_result returned by a miniaudio call.
 * @param context A descriptive string identifying the call site.
 * @throws std::runtime_error if result is not MA_SUCCESS.
 */
inline void ma_check(ma_result result, const std::string& context) {
    if (result != MA_SUCCESS) {
        throw std::runtime_error("[sfxcoopa] miniaudio error in " + context +
                                  ": " + ma_result_description(result));
    }
}

}  // namespace util
}  // namespace sfx
}  // namespace coopa

/**
 * @brief Wraps a miniaudio call and throws std::runtime_error on failure.
 *
 * Usage: SFX_MA_CHECK(ma_decoder_init_file(...));
 */
#define SFX_MA_CHECK(expr) \
    ::coopa::sfx::util::ma_check((expr), #expr)

#endif  // SFXCOOPA_UTIL_ERROR_H
