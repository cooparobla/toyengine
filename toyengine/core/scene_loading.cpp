#include <toyengine/core/scene_loading.h>

#include <coopa/scene/scene.h>
#include <toyengine/ui/ui_assets.h>
#include <uicoopa/immediate/imm.h>

#include <algorithm>

namespace toy {
namespace core {

SceneTransition SceneTransition::none() {
    SceneTransition t;
    t.kind = Kind::None;
    t.fade_out = t.fade_in = 0.0f;
    return t;
}

SceneTransition SceneTransition::fade(float seconds, glm::vec3 color) {
    SceneTransition t;
    t.kind = Kind::Fade;
    t.color = color;
    t.fade_out = t.fade_in = std::max(0.0f, seconds);
    return t;
}

SceneTransition SceneTransition::loading_screen_ui(std::string ui, float fade_seconds,
                                         glm::vec3 color) {
    SceneTransition t = fade(fade_seconds, color);
    t.kind = Kind::LoadingScreen;
    t.loading_screen = std::move(ui);
    return t;
}

std::optional<SceneTransition::Kind> SceneTransition::parse_kind(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "none") return Kind::None;
    if (s == "fade") return Kind::Fade;
    if (s == "loading_screen" || s == "loadingscreen") return Kind::LoadingScreen;
    return std::nullopt;
}

} // namespace core
} // namespace toy

namespace toy {
namespace core {
namespace detail {

void SceneLoadState::complete(coopa::scene::Scene& s) {
    scene = &s;
    progress = 1.0f;
    auto callbacks = std::move(on_complete);
    on_complete.clear();
    for (auto& fn : callbacks) if (fn) fn(s);
    on_failed.clear();
}

void SceneLoadState::fail(std::string why) {
    stage = SceneLoadStage::Failed;
    error = std::move(why);
    builder.reset();
    read.reset();
    auto callbacks = std::move(on_failed);
    on_failed.clear();
    for (auto& fn : callbacks) if (fn) fn(error);
    on_complete.clear();
}

} // namespace detail
} // namespace core
} // namespace toy

namespace toy {
namespace core {

const std::string& SceneLoadHandle::error() const { static const std::string none; return state_ ? state_->error : none; }

const std::string& SceneLoadHandle::path() const { static const std::string none; return state_ ? state_->path : none; }

void SceneLoadHandle::on_complete(std::function<void(coopa::scene::Scene&)> fn) const {
    if (!state_ || !fn) return;
    if (state_->activated()) { fn(*state_->scene); return; }
    if (state_->stage != SceneLoadStage::Failed) state_->on_complete.push_back(std::move(fn));
}

void SceneLoadHandle::on_failed(std::function<void(const std::string&)> fn) const {
    if (!state_ || !fn) return;
    if (state_->stage == SceneLoadStage::Failed) { fn(state_->error); return; }
    if (!state_->activated()) state_->on_failed.push_back(std::move(fn));
}

coopa::scene::Scene& SceneTransitionLayer::scene() {
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

void SceneTransitionLayer::set_fade(const glm::vec3& color, float alpha) {
    color_ = color;
    alpha_ = std::clamp(alpha, 0.0f, 1.0f);
}

bool SceneTransitionLayer::show_loading_screen(const std::string& ref) {
    hide_loading_screen();
    ui_root_ = toy::ui::spawn(scene(), ref);
    return ui_root_ != nullptr;
}

void SceneTransitionLayer::hide_loading_screen() {
    if (ui_root_ && scene_) toy::ui::close(*scene_, ui_root_);
    ui_root_ = nullptr;
}

void SceneTransitionLayer::update_loading_screen(float progress, const std::string& status) {
    if (!ui_root_) return;
    coopa::ui::UiHandle ui(ui_root_);
    if (ui.has("progress")) ui.set("progress", progress);
    if (ui.has("status")) ui.set_text("status", status);
}

void SceneTransitionLayer::release() {
    ui_root_ = nullptr;
    canvas_ = nullptr;
    scene_.reset();
    alpha_ = 0.0f;
}

namespace {

/// How much of the bar the stages that build a scene fill before the warm-up takes the rest.
/// The two halves split it because neither is a knowable fraction of the other, and a bar that
/// filled completely before warming began would have nothing left to show for the seconds a
/// streamed world spends assembling itself.
constexpr float k_warm_bar_start = 0.5f;

/// How fast the bar is allowed to travel, in fractions of its width per second -- so it takes
/// at least 1/k_bar_max_rate seconds to cross. Fast enough to keep up with any load that is
/// worth covering, slow enough that a step in the figure behind it becomes travel.
constexpr float k_bar_max_rate = 0.8f;

}  // namespace

bool fold_warmup_states(detail::SceneLoadState& st,
                        const std::vector<coopa::scene::WarmupState>& states) {
    // Every installed system is asked; the ones that stream something answer. Warm when all of
    // them are, and the status names whichever has the most left to do -- so the line reads
    // "Building terrain..." while that is the long pole, then moves on.
    //
    // Progress is a work-weighted mean over every system that has participated so far
    // (SceneLoadState::warm_participants), not a flat mean over the ones participating this
    // frame. All three of those differences are what keeps the bar from lurching:
    //
    //   * A flat mean rescales itself as systems join. Water does not participate until a body
    //     exists to bake, so the denominator climbs over the opening frames and a share of the
    //     bar's width comes off it at every step.
    //   * A system's own progress falls back when it finds more to do -- water's pending count
    //     climbs as bodies come into sim_radius, the navmesh baker halves whenever a tile goes
    //     dirty. Each system's high-water mark is monotonic where its reading is not.
    //   * Weighting by work (the most items it has ever had outstanding) stops a system with
    //     four dirty navmesh tiles from moving the bar as far as one with four hundred chunks
    //     left to mesh. An equal share per system is what let small, late work swing it.
    int worst_remaining = -1;
    bool all_warm = true;
    const char* worst_what = "";
    for (const coopa::scene::WarmupState& ws : states) {
        if (!ws.participates) continue;
        detail::WarmParticipant& p = st.warm_participants[ws.name];
        p.peak_remaining = std::max(p.peak_remaining, ws.remaining);
        p.progress = std::max(p.progress, ws.warm ? 1.0f : std::clamp(ws.progress, 0.0f, 1.0f));
        if (!ws.warm) all_warm = false;
        if (!ws.warm && ws.remaining > worst_remaining) {
            worst_remaining = ws.remaining;
            worst_what = ws.what;
        }
    }
    // Nothing streams: there is nothing to wait for. Asked of the participants seen so far
    // rather than this frame's, so the opening frames -- before any system has walked its work
    // -- do not read as done.
    if (st.warm_participants.empty()) return true;
    float weighted = 0.0f;
    float weight = 0.0f;
    for (const auto& [name, p] : st.warm_participants) {
        // A system that has not counted its work yet still carries a share, so one that is
        // merely slow to look does not pin the bar at zero.
        const float w = static_cast<float>(std::max(p.peak_remaining, 1));
        weighted += p.progress * w;
        weight += w;
    }
    st.warm_progress = weighted / weight;
    if (!all_warm && worst_what[0] != '\0') {
        st.warm_status = std::string("Building ") + worst_what + "...";
    }
    if (all_warm) st.warm_progress = 1.0f;
    return all_warm;
}

float load_bar_target(const detail::SceneLoadState& st) {
    if (st.stage == SceneLoadStage::Warming) {
        const float start = st.warm_only ? 0.0f : k_warm_bar_start;
        return start + (1.0f - start) * std::clamp(st.warm_progress, 0.0f, 1.0f);
    }
    if (st.activated()) return 1.0f;   // warmed, or never warming: there is nothing left to load
    const bool warms = st.options.warm_up &&
                       st.options.transition.kind != SceneTransition::Kind::None;
    return warms ? st.progress * k_warm_bar_start : st.progress;
}

float advance_load_bar(float current, float target, float dt) {
    if (target <= current) return current;
    return std::min(target, current + k_bar_max_rate * std::max(dt, 0.0f));
}

const char* scene_load_status_text(SceneLoadStage stage) {
    switch (stage) {
        case SceneLoadStage::ReadingDocument: return "Reading scene...";
        case SceneLoadStage::Building:        return "Building scene...";
        case SceneLoadStage::LoadingAssets:   return "Loading assets...";
        case SceneLoadStage::Warming:         return "Building world...";
        case SceneLoadStage::WaitingToSwap:
        case SceneLoadStage::FadingIn:
        case SceneLoadStage::Done:            return "Ready";
        case SceneLoadStage::Failed:          return "Failed";
    }
    return "";
}

} // namespace core
} // namespace toy
