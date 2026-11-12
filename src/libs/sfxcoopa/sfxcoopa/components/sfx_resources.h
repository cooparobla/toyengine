/**
 * @file sfx_resources.h
 * @brief SfxResources: the process-wide engine handle and asset search roots
 * the scene components (AudioSource / AudioListener) and their YAML parsers
 * consult, since SceneLoader parser callbacks and Component::update() get no
 * engine handle of their own. Mirrors uicoopa/ui_yaml.h's UIResources.
 *
 * Non-owning: the host constructs the AudioEngine, calls set_engine(&engine)
 * before starting any scene that uses audio components, and must call
 * set_engine(nullptr) before destroying the engine if any scene with audio
 * components outlives it (components compare their cached engine against
 * engine() before touching it on destruction).
 */

#ifndef SFXCOOPA_COMPONENTS_SFX_RESOURCES_H
#define SFXCOOPA_COMPONENTS_SFX_RESOURCES_H

#include <sfxcoopa/core/engine.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace coopa {
namespace sfx {

/**
 * @class SfxResources
 * @brief Process-wide engine pointer + clip path resolver + listener bookkeeping.
 *
 * @code
 * coopa::sfx::core::AudioEngine engine(format);
 * auto& res = coopa::sfx::SfxResources::instance();
 * res.set_engine(&engine);
 * res.add_search_root(project_assets_dir);
 * coopa::sfx::register_sfx_components();
 * // per frame, after scene update:
 * if (!res.consume_listener_pushed()) coopa::sfx::components::apply_listener_matrix(engine, camera_world);
 * engine.update(dt);
 * @endcode
 */
class SfxResources {
public:
    static SfxResources& instance() {
        static SfxResources resources;
        return resources;
    }

    /** @brief Sets (or clears, with nullptr) the engine every audio component plays through. */
    void set_engine(coopa::sfx::core::AudioEngine* engine) { engine_ = engine; }
    coopa::sfx::core::AudioEngine* engine() const { return engine_; }

    /** @brief Appends an asset root that relative clip paths are tried against (in insertion order). */
    void add_search_root(const std::string& root) {
        for (const auto& r : roots_) if (r == root) return;
        roots_.push_back(root);
    }
    void set_search_roots(std::vector<std::string> roots) { roots_ = std::move(roots); }
    void clear_search_roots() { roots_.clear(); }
    const std::vector<std::string>& search_roots() const { return roots_; }

    /**
     * @brief Resolves a clip path: absolute paths are returned as-is; a relative
     * path that exists as given (relative to the working directory, or already
     * resolved by SceneLoader's ParseContext) is returned unchanged; otherwise
     * each search root is tried in order and the first existing candidate wins.
     * Falls through to `path` unchanged when nothing exists, so the eventual
     * load failure names the path the author wrote.
     */
    std::string resolve(const std::string& path) const {
        if (path.empty()) return path;
        const std::filesystem::path p(path);
        if (p.is_absolute()) return path;
        std::error_code ec;
        if (std::filesystem::exists(p, ec)) return path;
        for (const auto& root : roots_) {
            const std::filesystem::path candidate = std::filesystem::path(root) / p;
            if (std::filesystem::exists(candidate, ec)) return candidate.string();
        }
        return path;
    }

    /** @brief Called by AudioListener::update() each time it pushes a pose into the engine. */
    void mark_listener_pushed() { listener_pushed_ = true; }

    /**
     * @brief True if any AudioListener pushed the engine listener since the previous
     * call; clears the flag. Hosts call this once per frame after the scene update
     * and drive the listener from their camera (apply_listener_matrix) when it's false.
     */
    bool consume_listener_pushed() {
        const bool pushed = listener_pushed_;
        listener_pushed_ = false;
        return pushed;
    }

    /** @brief Number of live AudioListener components (any active state). Informational. */
    int listener_count() const { return listener_count_; }
    void register_listener_() { ++listener_count_; }
    void unregister_listener_() { --listener_count_; }

private:
    coopa::sfx::core::AudioEngine* engine_ = nullptr;
    std::vector<std::string> roots_;
    bool listener_pushed_ = false;
    int listener_count_ = 0;
};

}  // namespace sfx
}  // namespace coopa

#endif  // SFXCOOPA_COMPONENTS_SFX_RESOURCES_H
