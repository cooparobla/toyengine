/**
 * @file loading_config.h
 * @brief The `loading:` section of assets/config.yaml: the project-global defaults every scene
 *        load and Engine::begin_scene_warmup() cover with, before a scene's own
 *        `settings.loading:` block (see scene_loading.h) overrides a key.
 */

#ifndef TOYENGINE_CORE_LOADING_CONFIG_H
#define TOYENGINE_CORE_LOADING_CONFIG_H

#include <glm/glm.hpp>

#include <string>

namespace toy {
namespace core {

/** @brief Which corner of the screen the built-in loading mark sits in. */
enum class LoadingCorner {
    BottomRight,
    BottomLeft,
    TopRight,
    TopLeft,
};

/** @brief "bottom_right" / "bottom_left" / "top_right" / "top_left" (case-insensitive). */
LoadingCorner parse_loading_corner(const std::string& value);
/** @brief Inverse of parse_loading_corner(), for round-tripping into YAML. */
const char* loading_corner_name(LoadingCorner corner);

/**
 * @struct LoadingConfig
 * @brief The `loading:` section: whether the engine ever shows a loading state, and how the
 *        built-in screen looks.
 *
 * Layering is `LoadingConfig` defaults < config.yaml's `loading:` < a scene's own
 * `settings.loading:` (see scene_load_options() / apply_scene_loading_block() in
 * scene_loading.h) -- the same pattern as render/physics/navigation scene overrides, except
 * this section is deliberately NOT in AppConfig::kSceneSettingsSections: a scene overriding
 * `loading.min_display_time` must not re-derive the renderer, which is what going through
 * AppConfig::with_scene_settings() would do.
 */
struct LoadingConfig {
    /// Launch (and every scene load / Engine::begin_scene_warmup()) covers with a loading
    /// state and holds it until every warm-up participant reports warm. False restores the
    /// old opt-in behaviour: no automatic cover unless a scene asks for one.
    bool enabled = true;
    /// Draw the built-in cycling-block-faces mark. False leaves only the background fill (and
    /// an overlay_ui, if one is set) -- for a project that wants to own the whole screen.
    bool spinner = true;
    /// Display-referred RGB behind the mark. Pure black.
    glm::vec3 background{0.0f};
    /// The mark's tint. Pure white.
    glm::vec3 spinner_color{1.0f};
    /// Which corner the mark sits in.
    LoadingCorner corner = LoadingCorner::BottomRight;
    /// A UI screen asset (assets/ui/<name>.yaml) drawn over the mark. Blank: none -- the
    /// built-in screen alone, which is the out-of-the-box look every project starts with.
    std::string overlay_ui;
    /// The cover stays up at least this long (seconds from the request), so a load that
    /// happens to be fast never flashes the screen for a frame or two.
    float min_display_time = 0.0f;
    /// Seconds to fade from the cover into the revealed scene.
    float fade_in = 0.35f;
    /// Seconds to fade the cover in over the scene being left.
    float fade_out = 0.35f;
    /// Reveal anyway this many seconds into warming, so a system that never reports warm
    /// cannot hang the game. 0 waits indefinitely.
    float warm_up_timeout = 20.0f;
    /// Main-thread milliseconds per frame the warming scene may spend on streaming work while
    /// covered -- see SceneLoadOptions::warm_up_budget_ms for why this is bounded, not lifted.
    float warm_up_budget_ms = 8.0f;
};

} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_LOADING_CONFIG_H
