/**
 * @file config.h
 * @brief Runtime configuration loaded from assets/config.yaml.
 *
 * Mirrors blendy's blendy::core::AppConfig pattern (see
 * blendy/src/blendy/core/config.h): every field has an in-class default, every
 * YAML key is individually optional, unknown keys are silently ignored, and a
 * missing or malformed file falls back to defaults rather than failing
 * startup. The file is read through coopa::yaml::load_document(), so a
 * packaged config.caml (see toyengine/core/caml_codec.h) loads the same way
 * as config.yaml, and a config.yaml path finds config.caml when only the
 * encoded file exists.
 */

#ifndef TOYENGINE_CORE_CONFIG_H
#define TOYENGINE_CORE_CONFIG_H

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

#include <coopa/scene/config.h>
#include <physxcoopa/nav/nav_settings.h>
#include <toyengine/render/toy_render_config.h>

namespace toy {
namespace core {

/**
 * @struct WindowConfig
 * @brief Window and presentation settings.
 */
struct WindowConfig {
    std::string title  = "toyengine";
    uint32_t    width  = 1920;
    uint32_t    height = 1080;
    bool        vsync  = true;
    /**
     * False renders into a window that is never mapped: no window appears, and nothing can
     * take focus or grab the pointer (Engine::apply_cursor_capture_() also stands down, since
     * an invisible window has no business hiding the cursor). Frames are otherwise identical,
     * which is what makes it the mode the headless test suite runs in -- see
     * coopa::gfx::presentation::Window's constructor.
     */
    bool        visible = true;
};

/**
 * @struct OutputConfig
 * @brief Headless frame export configuration.
 */
struct OutputConfig {
    bool        save_on_exit  = true;
    std::string filepath      = "./output/frame.png";
    /**
     * True saves the internal low-resolution buffer 1:1 (pixel-perfect, but BEFORE any
     * display-resolution-only effect such as tilt shift). False saves the final image
     * actually shown in the window, at display resolution. See Engine::save_screenshot().
     */
    bool        save_low_res  = true;
};

/**
 * @struct AudioConfig
 * @brief The `audio:` section: output device and default bus volumes (a player's own volume
 *        choices, saved in UserSettings, override these).
 */
struct AudioConfig {
    bool        enabled     = true;        ///< False: no device at all (everything silent).
    uint32_t    sample_rate = 48000;
    std::string device      = "default";   ///< "default", or "null" (no sound card; still mixes).
    float       master      = 1.0f;
    float       music       = 0.8f;
    float       sfx         = 1.0f;
    float       ui          = 0.8f;
};

/**
 * @struct JobsConfig
 * @brief Job-system sizing and the per-frame parallel-dispatch threshold.
 */
struct JobsConfig {
    /// Worker threads for the shared JobEngine. 0 = std::thread::hardware_concurrency().
    unsigned int worker_threads = 0;
    /**
     * Minimum element count before a per-frame loop dispatches jobs instead of
     * running serially. Deliberately low so the pixel demo (8 MeshRenderers,
     * 4 SdfRenderers) exercises the parallel path; libcoopa's own house value for
     * production-sized workloads is 256 (see AnimationSystem::parallel_threshold_).
     */
    std::size_t parallel_threshold = 4;
};

/**
 * @struct DebugConfig
 * @brief The `debug:` section: developer aids that ship off.
 */
struct DebugConfig {
    /// The on-screen stats overlay at startup: "off", "fps" or "full" (F3 cycles it at runtime).
    /// See toyengine/debug/debug_overlay.h; a TOY_SHIPPING build ignores it unless compiled
    /// with TOY_DEBUG_OVERLAY.
    std::string overlay = "off";
};

/**
 * @struct ParticlesConfig
 * @brief The `particles:` section (toyengine/particles/).
 */
struct ParticlesConfig {
    /// Global kill switch for `simulation: gpu` ParticleSystems: false (or a device without
    /// compute) simulates every system on the CPU.
    bool gpu_enabled = true;
};

/**
 * @struct SaveConfig
 * @brief The `save:` section (toyengine/save/save_system.h). Slots live in the per-user data
 *        directory (<user_data_dir>/saves); what goes in them is the game's.
 */
struct SaveConfig {
    /// How slot files are written: "auto" (caml in a TOY_SHIPPING build, plain YAML otherwise),
    /// "yaml" or "caml". Reading accepts either.
    std::string encode = "auto";
    /// The slot F5 saves to and F9 loads (the quick_save / quick_load input actions); empty
    /// turns the keys off.
    std::string quick_slot = "quicksave";
};

/**
 * @brief Parses one quality-tier string from config.yaml.
 *
 * Accepts `low`, `med`, `medium`, `high` and `ultra`; anything else falls back to
 * `High` (the shipped defaults), matching the config loader's every-key-optional,
 * never-fail policy.
 *
 * @param value The YAML string value of a `*_quality` key.
 * @return The corresponding render::RenderQuality tier.
 */
render::RenderQuality parse_render_quality(const std::string& value);

/**
 * @struct AppConfig
 * @brief Aggregated runtime configuration for toyengine.
 */
struct AppConfig {
    coopa::scene::SceneConfig         scene;
    WindowConfig                      window;
    render::ToyRenderConfig         render;
    OutputConfig                      output;
    AudioConfig                       audio;
    JobsConfig                        jobs;
    DebugConfig                       debug;
    SaveConfig                        save;
    ParticlesConfig                   particles;
    coopa::physx::util::PhysicsSettings physics;
    /// Navigation build + runtime settings (physxcoopa/nav/nav_settings.h). `navigation.enabled`
    /// false skips the system entirely; a scene with no colliders builds nothing either way.
    coopa::physx::nav::NavSettings navigation;

    /// The document this config was parsed from (config.yaml), kept so a scene's `settings:`
    /// overrides can be layered on at the YAML level -- see with_scene_settings(). Empty for a
    /// config built in code.
    fkyaml::node source = fkyaml::node::mapping();

    /**
     * @brief Loads application configuration from a YAML file.
     * @param path Path to the configuration file (e.g. assets/config.yaml).
     * @return Loaded AppConfig, with any missing/malformed fields left at default.
     */
    static AppConfig load(const std::string& path);

    /**
     * @brief Parses an already-loaded config document -- AppConfig::load() minus the file read.
     *        The editor uses this to apply an edited, unsaved config.yaml to a live Engine.
     */
    static AppConfig from_node(const fkyaml::node& root);

    /**
     * @brief This config with a scene's `settings:` overrides applied -- the config a scene
     *        actually runs with.
     *
     * Scenes may override the `render`, `physics` and `navigation` sections of config.yaml key
     * by key:
     * @code
     * scene:
     *   settings:
     *     render: { fog_density: 0.08, sky_intensity: 1.4 }
     *     physics: { gravity: { x: 0, y: 0, z: -4.0 } }
     * @endcode
     * The merge happens on the documents (config.yaml + overrides, then parsed), not on the
     * parsed structs, so quality presets resolve exactly as if the keys were in config.yaml:
     * a scene overriding `shadow_quality` re-runs that preset beneath config.yaml's own
     * explicit keys. Only render, physics and navigation are taken from the merge; every other section,
     * and anything set on this config in code rather than in its source document, is kept.
     * With no overrides this config is returned unchanged.
     */
    AppConfig with_scene_settings(const fkyaml::node& settings) const;

    /// The config.yaml sections a scene's `settings:` may override.
    static constexpr const char* kSceneSettingsSections[] = {"render", "physics", "navigation"};

    /** @brief True if `settings` overrides at least one key of an overridable section. */
    static bool has_scene_overrides(const fkyaml::node& settings);
};

} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_CONFIG_H
