#include <toyengine/core/scene_loading.h>

#include <coopa/scene/scene.h>
#include <toyengine/ui/ui_assets.h>
#include <uicoopa/immediate/imm.h>

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

const char* scene_load_status_text(SceneLoadStage stage) {
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

} // namespace core
} // namespace toy
