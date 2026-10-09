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
 *
 * The transition covers the swap: a fade to a colour and back, a loading-screen UI asset over
 * that colour (its `progress` StatBar / ProgressBar and `status` Text are filled in
 * automatically), or nothing. It draws on an engine overlay layer, inside the display rect.
 *
 * @code
 * toy::core::SceneLoadOptions opts;
 * opts.transition = toy::core::SceneTransition::loading_screen_ui("ui/loading_screen");
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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <uicoopa/binding/ui_handle.h>
#include <uicoopa/immediate/imm.h>
#include <uicoopa/immediate/imm_canvas.h>

#include <toyengine/ui/ui_assets.h>

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
        LoadingScreen,  ///< Fade to `color`, show the `loading_screen` UI until ready, fade in.
    };
    Kind        kind = Kind::Fade;
    glm::vec3   color{0.0f};
    float       fade_out = 0.35f;   ///< Seconds to cover the old scene.
    float       fade_in = 0.35f;    ///< Seconds to uncover the new one.
    std::string loading_screen;     ///< UI asset ("ui/loading_screen") for Kind::LoadingScreen.

    static SceneTransition none() {
        SceneTransition t;
        t.kind = Kind::None;
        t.fade_out = t.fade_in = 0.0f;
        return t;
    }
    static SceneTransition fade(float seconds = 0.35f, glm::vec3 color = glm::vec3(0.0f)) {
        SceneTransition t;
        t.kind = Kind::Fade;
        t.color = color;
        t.fade_out = t.fade_in = std::max(0.0f, seconds);
        return t;
    }
    static SceneTransition loading_screen_ui(std::string ui, float fade_seconds = 0.25f,
                                             glm::vec3 color = glm::vec3(0.0f)) {
        SceneTransition t = fade(fade_seconds, color);
        t.kind = Kind::LoadingScreen;
        t.loading_screen = std::move(ui);
        return t;
    }

    /** @brief "none" / "fade" / "loading_screen" (case-insensitive); nullopt otherwise. */
    static std::optional<Kind> parse_kind(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (s == "none") return Kind::None;
        if (s == "fade") return Kind::Fade;
        if (s == "loading_screen" || s == "loadingscreen") return Kind::LoadingScreen;
        return std::nullopt;
    }
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
};

/** @brief Where an async load is. Advances in this order; Failed can follow any stage. */
enum class SceneLoadStage {
    ReadingDocument,  ///< Reading + decoding + resolving inheritance on a worker.
    Building,         ///< Building objects on the main thread, a batch per frame.
    LoadingAssets,    ///< Waiting for the asset loads the build started.
    WaitingToSwap,    ///< Ready; waiting for the fade-out / min_display_time.
    FadingIn,         ///< Activated: the new scene runs, the transition fades away.
    Done,             ///< Activated and the transition is gone.
    Failed,           ///< The document or a parser failed (or a newer load cancelled it).
};

namespace detail {

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

    std::vector<std::function<void(coopa::scene::Scene&)>> on_complete;
    std::vector<std::function<void(const std::string&)>>   on_failed;

    bool activated() const { return scene != nullptr; }
    bool finished() const { return stage == SceneLoadStage::Done || stage == SceneLoadStage::Failed; }
    /// Still before activation (the request can be cancelled).
    bool pending() const { return !activated() && stage != SceneLoadStage::Failed; }

    /** @brief Raises progress (it never goes back). */
    void raise_progress(float p) { progress = std::max(progress, std::clamp(p, 0.0f, 1.0f)); }

    void complete(coopa::scene::Scene& s) {
        scene = &s;
        progress = 1.0f;
        auto callbacks = std::move(on_complete);
        on_complete.clear();
        for (auto& fn : callbacks) if (fn) fn(s);
        on_failed.clear();
    }
    void fail(std::string why) {
        stage = SceneLoadStage::Failed;
        error = std::move(why);
        builder.reset();
        read.reset();
        auto callbacks = std::move(on_failed);
        on_failed.clear();
        for (auto& fn : callbacks) if (fn) fn(error);
        on_complete.clear();
    }
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
    bool failed() const { return state_ && state_->stage == SceneLoadStage::Failed; }
    const std::string& error() const { static const std::string none; return state_ ? state_->error : none; }
    SceneLoadStage stage() const { return state_ ? state_->stage : SceneLoadStage::Failed; }
    /// The resolved scene file being loaded.
    const std::string& path() const { static const std::string none; return state_ ? state_->path : none; }
    /// The new scene, once is_ready(); null before.
    coopa::scene::Scene* scene() const { return state_ ? state_->scene : nullptr; }
    /// The transition's current cover (0 = the scene is fully visible, 1 = fully covered).
    float transition_alpha() const { return state_ ? state_->alpha : 0.0f; }

    /**
     * @brief Runs `fn` with the new scene at activation -- after it started and its systems
     *        were installed, before its first update (so it can restore state the player never
     *        sees change). Runs immediately if the scene is already live; never if it fails.
     */
    void on_complete(std::function<void(coopa::scene::Scene&)> fn) const {
        if (!state_ || !fn) return;
        if (state_->activated()) { fn(*state_->scene); return; }
        if (state_->stage != SceneLoadStage::Failed) state_->on_complete.push_back(std::move(fn));
    }
    /** @brief Runs `fn` with the error if the load fails (immediately if it already has). */
    void on_failed(std::function<void(const std::string&)> fn) const {
        if (!state_ || !fn) return;
        if (state_->stage == SceneLoadStage::Failed) { fn(state_->error); return; }
        if (!state_->activated()) state_->on_failed.push_back(std::move(fn));
    }

private:
    std::shared_ptr<detail::SceneLoadState> state_;
};

/**
 * @class SceneTransitionLayer
 * @brief The engine overlay scene a transition draws in: a full-rect colour fill (an
 *        immediate canvas) under an optional loading-screen UI asset.
 */
class SceneTransitionLayer {
public:
    /** @brief The layer's scene, built on first use. */
    coopa::scene::Scene& scene() {
        if (!scene_) {
            scene_ = std::make_unique<coopa::scene::Scene>("SceneTransition");
            // Far below a loading screen's own canvas (sort_order 0 by default).
            canvas_ = coopa::ui::build_immediate_canvas(*scene_, "SceneTransitionFade", -1000);
            canvas_->on_draw = [this](coopa::ui::imm::Context& ctx) {
                if (alpha_ <= 0.0f) return;
                const glm::vec2 size = ctx.canvas_size();
                ctx.fill(coopa::ui::imm::Box{0.0f, 0.0f, size.x, size.y}, glm::vec4(color_, alpha_));
            };
        }
        return *scene_;
    }
    bool has_scene() const { return scene_ != nullptr; }

    void set_fade(const glm::vec3& color, float alpha) {
        color_ = color;
        alpha_ = std::clamp(alpha, 0.0f, 1.0f);
    }
    float alpha() const { return alpha_; }

    /** @brief Spawns the loading-screen UI asset `ref` into the layer. False if it failed. */
    bool show_loading_screen(const std::string& ref) {
        hide_loading_screen();
        ui_root_ = toy::ui::spawn(scene(), ref);
        return ui_root_ != nullptr;
    }
    /** @brief Destroys the loading screen at the end of the layer's next late_update(). */
    void hide_loading_screen() {
        if (ui_root_ && scene_) toy::ui::close(*scene_, ui_root_);
        ui_root_ = nullptr;
    }
    bool loading_screen_visible() const { return ui_root_ != nullptr; }
    coopa::ui::UiHandle loading_screen() const { return coopa::ui::UiHandle(ui_root_); }

    /** @brief Fills the loading screen's `progress` bar (0..1) and `status` text, if it has them. */
    void update_loading_screen(float progress, const std::string& status) {
        if (!ui_root_) return;
        coopa::ui::UiHandle ui(ui_root_);
        if (ui.has("progress")) ui.set("progress", progress);
        if (ui.has("status")) ui.set_text("status", status);
    }

    /** @brief Destroys the scene (before the fonts and the device it draws with go away). */
    void release() {
        ui_root_ = nullptr;
        canvas_ = nullptr;
        scene_.reset();
        alpha_ = 0.0f;
    }

private:
    std::unique_ptr<coopa::scene::Scene> scene_;
    coopa::ui::ImmediateCanvas* canvas_ = nullptr;
    coopa::scene::SceneObject*  ui_root_ = nullptr;
    glm::vec3 color_{0.0f};
    float     alpha_ = 0.0f;
};

/** @brief Loading-screen status text for a stage. */
inline const char* scene_load_status_text(SceneLoadStage stage) {
    switch (stage) {
        case SceneLoadStage::ReadingDocument: return "Reading scene...";
        case SceneLoadStage::Building:        return "Building scene...";
        case SceneLoadStage::LoadingAssets:   return "Loading assets...";
        case SceneLoadStage::WaitingToSwap:
        case SceneLoadStage::FadingIn:
        case SceneLoadStage::Done:            return "Ready";
        case SceneLoadStage::Failed:          return "Failed";
    }
    return "";
}

}  // namespace core
}  // namespace toy

#endif  // TOYENGINE_CORE_SCENE_LOADING_H
