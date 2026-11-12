#include <toyengine/core/scene_loading.h>

#include <toyengine/core/loading_screen.h>

#include <coopa/asset/asset_manager.h>
#include <coopa/scene/scene.h>
#include <uicoopa/immediate/imm.h>
#include <uicoopa/text/font_defaults.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

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

SceneLoadOptions scene_load_options(const LoadingConfig& config) {
    SceneLoadOptions opts;
    if (!config.enabled) {
        opts.warm_up = false;
        opts.transition = SceneTransition::none();
        return opts;
    }
    opts.transition.kind = SceneTransition::Kind::LoadingScreen;
    opts.transition.color = config.background;
    opts.transition.fade_in = config.fade_in;
    opts.transition.fade_out = config.fade_out;
    opts.transition.loading_screen = config.overlay_ui;
    opts.warm_up = true;
    opts.warm_up_timeout = config.warm_up_timeout;
    opts.warm_up_budget_ms = config.warm_up_budget_ms;
    opts.min_display_time = config.min_display_time;
    return opts;
}

void apply_scene_loading_block(const fkyaml::node& settings, SceneLoadOptions& opts) {
    if (!settings.is_mapping() || !settings.contains("loading")) return;
    const fkyaml::node& node = settings.at("loading");
    if (!node.is_mapping()) return;

    bool kind_set = false;
    if (node.contains("ui")) {
        opts.transition.loading_screen = node.at("ui").get_value<std::string>();
        if (!opts.transition.loading_screen.empty()) {
            opts.transition.kind = SceneTransition::Kind::LoadingScreen;
            kind_set = true;
        }
    }
    if (node.contains("transition")) {
        if (auto parsed = SceneTransition::parse_kind(node.at("transition").get_value<std::string>())) {
            opts.transition.kind = *parsed;
            kind_set = true;
        } else {
            std::cerr << "[toyengine] scene settings: loading.transition must be none, fade or "
                         "loading_screen\n";
        }
    }
    if (node.contains("warm_up")) {
        opts.warm_up = node.at("warm_up").get_value<bool>();
    } else if (kind_set) {
        opts.warm_up = opts.transition.kind != SceneTransition::Kind::None;
    }
    if (node.contains("fade_in")) opts.transition.fade_in = node.at("fade_in").get_value<float>();
    if (node.contains("fade_out")) opts.transition.fade_out = node.at("fade_out").get_value<float>();
    if (node.contains("warm_up_timeout")) {
        opts.warm_up_timeout = node.at("warm_up_timeout").get_value<float>();
    }
    if (node.contains("warm_up_budget_ms")) {
        opts.warm_up_budget_ms = node.at("warm_up_budget_ms").get_value<float>();
    }
    if (node.contains("min_display_time")) {
        opts.min_display_time = node.at("min_display_time").get_value<float>();
    }
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
        canvas_ = coopa::ui::build_immediate_canvas(*scene_, "SceneTransition", -1000);
        // One canvas draws both, in this order: the fade is what the screen sits on, and
        // declaration order within a layer is draw order.
        canvas_->on_draw = [this](coopa::ui::imm::Context& ctx) {
            draw_fade_(ctx);
            if (loading_visible_) draw_loading_screen_(ctx);
        };
        // A font: the fade never needed one, the status line does. Whatever the host already
        // published, else the stock face from an asset root. Without one the bar still draws
        // and the status line simply does not, which is a reasonable way to degrade.
        coopa::ui::Font* font = coopa::ui::FontDefaults::font;
        if (!font && coopa::ui::FontDefaults::resolve_font) {
            for (const std::string& root : coopa::ui::FontDefaults::search_dirs) {
                const std::filesystem::path f =
                    std::filesystem::path(root) / "fonts" / "inter_regular.ttf";
                std::error_code ec;
                if (!std::filesystem::exists(f, ec)) continue;
                font = coopa::ui::FontDefaults::resolve_font(f.string());
                break;
            }
        }
        if (font) canvas_->context().text.set_font(font);
    }
    return *scene_;
}

void SceneTransitionLayer::draw_fade_(coopa::ui::imm::Context& ctx) {
    if (alpha_ <= 0.0f) return;
    ctx.fill(ctx.stretch(), glm::vec4(color_, alpha_));
}

void SceneTransitionLayer::draw_loading_screen_(coopa::ui::imm::Context& ctx) {
    // The FADE's colour at full opacity, not style_.background: the screen has to cover
    // whatever is mid-teardown behind it, and it has to agree with the fade it appears out of.
    // A different background would be a second colour arriving on top of the fade -- visible
    // as a flash. scene_load_options() sets the fade's colour from style_.background for
    // exactly this reason, so in practice the two already agree.
    ctx.fill(ctx.stretch(), glm::vec4(color_, 1.0f));

    // The mark alone says work is in progress; no text, no bar. It draws straight into the
    // Context's own DrawList, which is already begun for the fade fill above.
    emit_loading_screen(ctx.draw_list(), ctx.canvas_size(), style_, static_cast<float>(ctx.time()));

    // The project's overlay, if one named itself and loaded, drawn on top of the mark.
    const toy::ui::UiPanel* overlay = overlay_panel_ ? overlay_panel_.get() : overlay_handle_.get();
    if (overlay) {
        const coopa::ui::imm::Box b = ctx.anchor(overlay->anchor, overlay->offset, overlay->size);
        toy::ui::draw_panel(ctx, b, *overlay, overlay_bindings_, &overlay_tab_);
    }
}

void SceneTransitionLayer::set_fade(const glm::vec3& color, float alpha) {
    color_ = color;
    alpha_ = std::clamp(alpha, 0.0f, 1.0f);
}

bool SceneTransitionLayer::show_loading_screen(const std::string& ref) {
    scene();   // the canvas has to exist before anything can be drawn in it
    progress_ = 0.0f;
    status_.clear();
    load_overlay_(ref);
    loading_visible_ = true;
    return true;
}

void SceneTransitionLayer::hide_loading_screen() {
    loading_visible_ = false;
    progress_ = 0.0f;
    status_.clear();
    overlay_handle_ = {};
    overlay_panel_.reset();
}

void SceneTransitionLayer::update_loading_screen(float progress, const std::string& status) {
    progress_ = std::clamp(progress, 0.0f, 1.0f);
    status_ = status;
}

void SceneTransitionLayer::load_overlay_(const std::string& ref) {
    overlay_handle_ = {};
    overlay_panel_.reset();
    overlay_tab_ = 0;
    if (ref.empty()) return;

    // `.yaml` is optional in config.yaml / a scene's settings, so `overlay_ui: ui/loading_screen`
    // works; both load paths below want the real filename.
    const bool has_ext = ref.size() > 5 && ref.rfind(".yaml") == ref.size() - 5;
    const std::string rel = has_ext ? ref : ref + ".yaml";

    // Through the asset system when a host has published one (set_panel_assets()) -- the same
    // seam PanelScreen uses, so the file is parsed once and hot-reloads in the editor.
    if (coopa::asset::AssetManager* assets = toy::ui::panel_assets()) {
        overlay_handle_ = assets->load<toy::ui::UiPanel>(rel);
        if (!overlay_handle_.get()) {
            std::cerr << "[toyengine] Loading screen overlay '" << rel << "' did not load\n";
        }
        return;
    }

    // No host: read the file directly, same as PanelScreen's headless fallback. A missing or
    // malformed asset leaves the built-in mark up on its own, which is the one thing a loading
    // screen must never fail to do.
    for (const std::string& root : coopa::ui::FontDefaults::search_dirs) {
        const std::filesystem::path p = std::filesystem::path(root) / rel;
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) continue;
        try {
            std::ifstream f(p);
            std::ostringstream ss;
            ss << f.rdbuf();
            overlay_panel_ = std::make_shared<toy::ui::UiPanel>(toy::ui::parse_panel(ss.str()));
        } catch (const std::exception& e) {
            std::cerr << "[toyengine] Loading screen overlay '" << ref << "' failed to parse ("
                      << e.what() << ")\n";
        }
        return;
    }
    std::cerr << "[toyengine] Loading screen overlay '" << ref << "' not found\n";
}

void SceneTransitionLayer::release() {
    loading_visible_ = false;
    canvas_ = nullptr;
    scene_.reset();
    overlay_handle_ = {};
    overlay_panel_.reset();
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
