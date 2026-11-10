/**
 * @file shader_library.h
 * @brief Ordered-search-path resolver from a logical shader name to a
 *        compiled .spv path on disk.
 */

#ifndef COOPA_GFX_PIPELINE_SHADER_LIBRARY_H
#define COOPA_GFX_PIPELINE_SHADER_LIBRARY_H

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace coopa {
namespace gfx {
namespace pipeline {

/**
 * @class ShaderLibrary
 * @brief Resolves a logical shader name (e.g. "ssr_composite.frag") to the
 *        absolute path of its compiled .spv, searching an ordered list of
 *        directories and returning the first match.
 *
 * This is the runtime mirror of glslc's `-I` search order: an app's own
 * `assets/shaders/` is searched before gfxcoopa's (which holds the GI, SMAA
 * and test shaders gfxcoopa's own classes load), so an app-local shader of the
 * same logical name shadows gfxcoopa's copy exactly the way `#include "x"`
 * shadows `#include <gfx/x>` at compile time.
 *
 * The single-argument constructors are implicit, so anywhere a
 * `const ShaderLibrary&` is expected a plain `std::string` directory works
 * too, and means a one-directory search path.
 */
class ShaderLibrary {
public:
    ShaderLibrary() = default;

    /// Implicit on purpose: a bare directory string is a valid, and common,
    /// one-entry search path, so `f(..., "assets/shaders")` reads naturally.
    ShaderLibrary(std::string dir) : dirs_{std::move(dir)} {}
    ShaderLibrary(const char* dir) : dirs_{dir} {}

    explicit ShaderLibrary(std::vector<std::string> dirs) : dirs_(std::move(dirs)) {}

    /// Convenience constructor for the common two-tier case: an app's own
    /// shader directory searched first, then gfxcoopa's own assets/shaders/.
    /// `base_dir` is a required parameter rather than something this class
    /// tries to auto-detect -- gfxcoopa is header-only and has no reliable
    /// way to know where a downstream app checked out its gfxcoopa sibling;
    /// callers already have that path (e.g. via their own generated
    /// `PROJ_DIR` + "/gfxcoopa/assets/shaders") and should pass it explicitly.
    static ShaderLibrary app_over_base(std::string app_dir, std::string base_dir) {
        return ShaderLibrary(std::vector<std::string>{std::move(app_dir), std::move(base_dir)});
    }

    /**
     * @brief Resolves a logical shader name to an absolute .spv path.
     * @param logical_name Shader name with stage suffix but no ".spv" and no
     *                      directory, e.g. "ssr_composite.frag".
     * @return Absolute path to the first matching ".spv" found across dirs().
     * @throws std::runtime_error listing every directory searched, if none contain it.
     */
    const std::string& resolve(const std::string& logical_name) const {
        auto cached = cache_.find(logical_name);
        if (cached != cache_.end()) {
            return cached->second;
        }

        for (const auto& dir : dirs_) {
            std::string candidate = dir + "/" + logical_name + ".spv";
            if (std::filesystem::exists(candidate)) {
                auto [it, inserted] = cache_.emplace(logical_name, std::move(candidate));
                (void)inserted;
                return it->second;
            }
        }

        std::string msg = "[gfxcoopa] ShaderLibrary: could not resolve '" + logical_name +
                          ".spv' in any of:";
        for (const auto& dir : dirs_) {
            msg += "\n  " + dir;
        }
        throw std::runtime_error(msg);
    }

    /// Shorthand for resolve().
    const std::string& operator()(const std::string& logical_name) const {
        return resolve(logical_name);
    }

    /// @brief True if `logical_name` resolves in some searched directory.
    bool has(const std::string& logical_name) const {
        for (const auto& dir : dirs_) {
            if (std::filesystem::exists(dir + "/" + logical_name + ".spv")) {
                return true;
            }
        }
        return false;
    }

    /// @brief The ordered search path, app-provided directories first.
    const std::vector<std::string>& dirs() const { return dirs_; }

private:
    std::vector<std::string> dirs_;
    mutable std::map<std::string, std::string> cache_; /**< resolve() is logically const. */
};

} // namespace pipeline
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_PIPELINE_SHADER_LIBRARY_H
