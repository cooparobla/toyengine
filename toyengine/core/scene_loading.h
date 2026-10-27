/**
 * @file scene_loading.h
 * @brief Async scene loading: the options and handle Engine::load_scene_async() takes and
 *        returns, and the transition layer (fade / loading screen) drawn while it runs.
 *
 * A scene loads in stages while the current one keeps running and rendering:
 *   1. **Document** -- the file is read, decoded and its `inherit_from` / `prefab` references
 *      expanded on a worker job (SceneLoader::read_document_async()).
 *   2. **Build** -- objects are built on the main thread a few root objects per frame, within
 *      SceneLoadOptions::build_budget_ms (component parsers are main-thread only). The asset
 *      loads they start decode on workers meanwhile.
 *   3. **Assets** -- the engine waits for the pending asset loads, finalizing (GPU uploads) at
 *      most SceneLoadOptions::finalize_budget_ms of them per frame.
 *   4. **Activation** -- one GPU wait, the old scene is destroyed (unless `additive`), the new
 *      one is started, its systems installed and it begins simulating. on_complete fires here,
 *      before the new scene's first update.
 *   5. **Warming** (SceneLoadOptions::warm_up) -- the new scene updates and renders, so its
 *      systems build what they stream, while the transition stays at full cover with the loading
 *      screen up. A streamed world is not finished when its scene activates: terrain is still
 *      meshing, water still baking, the navmesh not yet built, and revealing the scene there
 *      shows the world assembling itself a frame at a time. Readiness comes from the scene's own
 *      systems (coopa::scene::ISceneSystem::warm_up_state()), so a scene warms on exactly what it
 *      installs; SceneLoadOptions::warm_up_timeout caps the wait so a system that never reports
 *      warm cannot hang the game.
 *
 * The transition covers the swap: a fade to a colour and back, a loading-screen UI asset over
 * that colour (its `progress` StatBar / ProgressBar and `status` Text are filled in
 * automatically), or nothing. It draws on an engine overlay layer, inside the display rect.
 *
 * @code
 * toy::core::SceneLoadOptions opts;
 * opts.transition = toy::core::SceneTransition::loading_screen_ui("ui/themes/game_dark");
 * auto load = engine.load_scene_async("scenes/level_2", opts);
 * load.on_complete([](coopa::scene::Scene& s) { ... });   // runs once the new scene is live
 * @endcode
 */

#ifndef TOYENGINE_CORE_SCENE_LOADING_H
#define TOYENGINE_CORE_SCENE_LOADING_H

#include <glm/glm.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_system.h>
#include <uicoopa/immediate/imm_canvas.h>


namespace toy {
namespace core {

/**
 * @struct SceneTransition
 * @brief What covers a scene change: a fade, a loading screen, or nothing.
 */
struct SceneTransition {
    enum class Kind {
        None,           ///< The new scene simply replaces the old one when it is ready.
        Fade,           ///< Fade to `color`, swap, fade back in.
        LoadingScreen,  ///< Fade to `color`, show a loading screen until ready, fade in.
    };
    Kind        kind = Kind::Fade;
    glm::vec3   color{0.0f};
    float       fade_out = 0.35f;   ///< Seconds to cover the old scene.
    float       fade_in = 0.35f;    ///< Seconds to uncover the new one.
    /**
     * @brief Theme the Kind::LoadingScreen screen is styled with ("ui/themes/game_dark").
     *
     * The screen itself is drawn in code (SceneTransitionLayer), so this only picks its look.
     * Non-empty is what asks for a screen at all; one that names no readable theme still
     * draws, in the built-in style.
     */
    std::string loading_screen;

    static SceneTransition none();
    static SceneTransition fade(float seconds = 0.35f, glm::vec3 color = glm::vec3(0.0f));
    static SceneTransition loading_screen_ui(std::string ui, float fade_seconds = 0.25f,
                                             glm::vec3 color = glm::vec3(0.0f));

    /** @brief "none" / "fade" / "loading_screen" (case-insensitive); nullopt otherwise. */
    static std::optional<Kind> parse_kind(std::string s);
};

/**
 * @struct SceneLoadOptions
 * @brief How Engine::load_scene_async() loads and swaps in a scene.
 */
struct SceneLoadOptions {
    SceneTransition transition;      ///< Default: a 0.35 s fade through black.
    /// Add the scene alongside the current ones (it updates with them; the active scene stays
    /// the active one) instead of replacing them.
    bool  additive = false;
    /// The transition stays up at least this long (seconds from the request), so a fast load
    /// does not flash a loading screen.
    float min_display_time = 0.0f;
    /// An object in the new scene to move the player (the first CharacterController's object)
    /// to before it starts. Empty leaves the player where the scene file puts it.
    std::string spawn_point;
    /// Main-thread milliseconds per frame spent building objects.
    float build_budget_ms = 4.0f;
    /// Main-thread milliseconds per frame spent finalizing asset loads while loading.
    float finalize_budget_ms = 4.0f;
    /// Hold the cover up after activation until the scene's systems report warm (stage 5 above).
    bool  warm_up = true;
    /// Reveal anyway this many seconds into warming, so a system that never warms cannot hang
    /// the game. 0 waits indefinitely.
    float warm_up_timeout = 20.0f;
    /// Main-thread milliseconds per frame the warming scene may spend on streaming work.
    ///
    /// Raised over the authored steady-state budgets (a chunk publish slice, a water bake slice)
    /// because nothing is visible yet, but deliberately still bounded: the loading screen is
    /// being drawn and animated out of these same frames, and an unbounded warm-up freezes it.
    float warm_up_budget_ms = 8.0f;
};

/** @brief Where an async load is. Advances in this order; Failed can follow any stage. */
enum class SceneLoadStage {
    ReadingDocument,  ///< Reading + decoding + resolving inheritance on a worker.
    Building,         ///< Building objects on the main thread, a batch per frame.
    LoadingAssets,    ///< Waiting for the asset loads the build started.
    WaitingToSwap,    ///< Ready; waiting for the fade-out / min_display_time.
    Warming,          ///< Activated and simulating, still covered, while its systems build.
    FadingIn,         ///< Activated: the new scene runs, the transition fades away.
    Done,             ///< Activated and the transition is gone.
    Failed,           ///< The document or a parser failed (or a newer load cancelled it).
};

namespace detail {

/**
 * @brief One warming system as the load follows it across frames.
 *
 * A system's own coopa::scene::WarmupState is a snapshot, and two of its fields move in ways a
 * bar cannot be driven by directly: `progress` falls back when the system finds more to do (a
 * water body enters range, a navmesh tile goes dirty), and `remaining` is not known at all until
 * the system has walked its work once. Keeping the high-water mark of both per system is what
 * turns those snapshots into something monotonic -- see fold_warmup_states().
 */
struct WarmParticipant {
    int   peak_remaining = 0;   ///< Most items this system has had outstanding: its weight.
    float progress = 0.0f;      ///< Its furthest reported progress, 0..1.
};

/** @brief Shared between the Engine and every SceneLoadHandle for one load. */
struct SceneLoadState {
    std::string      path;     ///< The resolved scene file.
    SceneLoadOptions options;
    SceneLoadStage   stage = SceneLoadStage::ReadingDocument;
    float            progress = 0.0f;
    float            elapsed = 0.0f;   ///< Seconds since the request (frame dt).
    float            alpha = 0.0f;     ///< The transition's current cover, 0..1.
    std::string      error;
    coopa::scene::Scene* scene = nullptr;   ///< Set at activation.
    uint64_t         frames = 0;           ///< Engine ticks since the request.

    std::shared_ptr<coopa::scene::SceneLoader::DocumentRead> read;
    std::unique_ptr<coopa::scene::SceneLoader::Builder>      builder;
    fkyaml::node     settings;            ///< The document's `scene.settings`.
    size_t           peak_pending = 0;    ///< Most asset loads seen pending since the request.
    bool             loading_screen_shown = false;
    float            warm_elapsed = 0.0f;  ///< Seconds spent in SceneLoadStage::Warming.
    uint64_t         warm_frames = 0;      ///< Frames spent there.
    float            warm_progress = 0.0f; ///< 0..1 across the warming systems.
    std::string      warm_status;          ///< "Building terrain...", from the slowest system.
    /// Every system that has participated in the warm-up, by WarmupState::name. Systems join it
    /// as they find work and stay in it once warm, so the set never shrinks under the mean.
    std::map<std::string, WarmParticipant> warm_participants;
    /// The figure the loading screen's bar is actually showing: travels toward the stage's
    /// progress at a bounded speed and never falls back. See advance_load_bar().
    float            display_progress = 0.0f;
    /// No load stages ran: the scene was already built and is only being warmed
    /// (Engine::begin_scene_warmup()), so the warm-up owns the whole bar rather than its tail.
    bool             warm_only = false;

    std::vector<std::function<void(coopa::scene::Scene&)>> on_complete;
    std::vector<std::function<void(const std::string&)>>   on_failed;

    bool activated() const { return scene != nullptr; }
    bool finished() const { return stage == SceneLoadStage::Done || stage == SceneLoadStage::Failed; }
    /// Still before activation (the request can be cancelled).
    bool pending() const { return !activated() && stage != SceneLoadStage::Failed; }

    /** @brief Raises progress (it never goes back). */
    void raise_progress(float p) { progress = std::max(progress, std::clamp(p, 0.0f, 1.0f)); }

    void complete(coopa::scene::Scene& s);
    void fail(std::string why);
};

}  // namespace detail

/**
 * @class SceneLoadHandle
 * @brief Watches one Engine::load_scene_async() request. Cheap to copy; every copy sees the
 *        same load. A default-constructed handle is empty (valid() false).
 */
class SceneLoadHandle {
public:
    SceneLoadHandle() = default;
    explicit SceneLoadHandle(std::shared_ptr<detail::SceneLoadState> state) : state_(std::move(state)) {}

    bool valid() const { return state_ != nullptr; }
    explicit operator bool() const { return valid(); }

    /// 0..1, never decreasing: the document, then the built objects, then the assets; 1 once
    /// the scene is activated.
    float progress() const { return state_ ? state_->progress : 0.0f; }
    /// True once the new scene is live (started, systems installed, simulating).
    bool is_ready() const { return state_ && state_->activated(); }
    /// True once the transition has also finished (or the load failed).
    bool is_done() const { return state_ && state_->finished(); }
    /// True while the scene is live and simulating but still covered, building what it streams.
    bool is_warming() const { return state_ && state_->stage == SceneLoadStage::Warming; }
    bool failed() const { return state_ && state_->stage == SceneLoadStage::Failed; }
    const std::string& error() const;
    SceneLoadStage stage() const { return state_ ? state_->stage : SceneLoadStage::Failed; }
    /// The resolved scene file being loaded.
    const std::string& path() const;
    /// The new scene, once is_ready(); null before.
    coopa::scene::Scene* scene() const { return state_ ? state_->scene : nullptr; }
    /// The transition's current cover (0 = the scene is fully visible, 1 = fully covered).
    float transition_alpha() const { return state_ ? state_->alpha : 0.0f; }

    /**
     * @brief Runs `fn` with the new scene at activation -- after it started and its systems
     *        were installed, before its first update (so it can restore state the player never
     *        sees change). Runs immediately if the scene is already live; never if it fails.
     */
    void on_complete(std::function<void(coopa::scene::Scene&)> fn) const;
    /** @brief Runs `fn` with the error if the load fails (immediately if it already has). */
    void on_failed(std::function<void(const std::string&)> fn) const;

private:
    std::shared_ptr<detail::SceneLoadState> state_;
};

/**
 * @class SceneTransitionLayer
 * @brief The engine overlay scene a transition draws in: a colour fill and, over it, an
 *        optional loading screen -- both declared per frame in one immediate canvas.
 */
class SceneTransitionLayer {
public:
    /** @brief The layer's scene, built on first use. */
    coopa::scene::Scene& scene();
    bool has_scene() const { return scene_ != nullptr; }

    void set_fade(const glm::vec3& color, float alpha);
    float alpha() const { return alpha_; }

    /**
     * @brief Starts drawing the loading screen, styled by the theme `ref` names.
     *
     * Always succeeds: a theme that cannot be read leaves the built-in style, because
     * the one thing a loading screen must never do is fail to appear. The bool is kept
     * for callers that still branch on it.
     */
    bool show_loading_screen(const std::string& ref);
    /** @brief Stops drawing the loading screen from the next frame. */
    void hide_loading_screen();
    bool loading_screen_visible() const { return loading_visible_; }

    /** @brief Sets the bar (0..1) and the line of text under it. */
    void update_loading_screen(float progress, const std::string& status);

    /** @brief Heading above the bar. Empty draws none. */
    std::string title = "Loading";

    /** @brief Destroys the scene (before the fonts and the device it draws with go away). */
    void release();

private:
    void draw_fade_(coopa::ui::imm::Context& ctx);
    void draw_loading_screen_(coopa::ui::imm::Context& ctx);

    std::unique_ptr<coopa::scene::Scene> scene_;
    coopa::ui::ImmediateCanvas* canvas_ = nullptr;
    /// Drawn by the canvas's own callback, so there is nothing to spawn and nothing to
    /// destroy -- which is why hide_loading_screen() takes effect on the next frame rather
    /// than at the end of a late_update().
    bool        loading_visible_ = false;
    float       progress_ = 0.0f;
    std::string status_;
    /// The look, from the theme show_loading_screen() was given. Default-constructed if it
    /// named none, which is a legible screen rather than an invisible one.
    coopa::ui::imm::Style loading_style_;
    glm::vec3 color_{0.0f};
    float     alpha_ = 0.0f;
};

/**
 * @brief Folds one frame of system reports into `st`'s warm-up progress and status line.
 *
 * Separated from the Engine (and from Scene::warm_up_states(), which it takes the reports from)
 * because what it does is arithmetic over the reports and the running state in `st` -- nothing
 * about it needs a device, a window or a scene to test.
 *
 * @param st     The load whose warm_progress, warm_status and warm_participants it advances.
 * @param states This frame's coopa::scene::ISceneSystem::warm_up_state() reports.
 * @return True when every participating system is warm -- the scene is worth showing.
 */
bool fold_warmup_states(detail::SceneLoadState& st,
                        const std::vector<coopa::scene::WarmupState>& states);

/**
 * @brief Where the loading screen's bar belongs for a load in its current stage: 0..1.
 *
 * One run across both halves of a load. The stages that build the scene fill the bar to
 * SceneLoadOptions::warm_up's share of it and the warm-up fills the rest, so a bar does not
 * arrive full with the seconds of world-building still ahead of it. A scene that was already
 * built and is only being warmed (Engine::begin_scene_warmup()) owns the whole bar.
 */
float load_bar_target(const detail::SceneLoadState& st);

/**
 * @brief Moves the bar from `current` toward `target` at a bounded speed: the value to draw.
 *
 * The figures behind a loading bar arrive in steps -- a batch of chunks publishes, an asset
 * finalizes, a system that was warm finds more to do -- and drawing each step as it lands reads
 * as snapping rather than loading. A speed limit turns them into travel: a target rising slower
 * than the limit is tracked exactly, and anything faster is closed at it. The limit is per
 * second, not per frame, so the motion is the same whatever the frame took -- which on a loading
 * screen is anything from a millisecond to a long meshing frame. Never returns less than
 * `current`: nothing can walk the bar backwards.
 */
float advance_load_bar(float current, float target, float dt);

/** @brief Loading-screen status text for a stage. */
const char* scene_load_status_text(SceneLoadStage stage);

}  // namespace core
}  // namespace toy

#endif  // TOYENGINE_CORE_SCENE_LOADING_H
