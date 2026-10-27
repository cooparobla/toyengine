#include <toyengine/env/env_preset.h>

#include <string>
#include <vector>

#include <iostream>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>

#include <coopa/animation/animation_system.h>
#include <coopa/animation/animator.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/scene/kinematic_mover.h>
#include <toyengine/weather/weather_system.h>

namespace toy {
namespace env {

void EnvPresetSwitcher::start() {
    // The panel is this component's own, drawn per frame, so there is nothing to subscribe to
    // and no widget to find: draw() reads `selected_` and reports a click straight back.
    ImmScreen::start();
}

void EnvPresetSwitcher::on_update(float) {
    if (!pending_initial_) return;
    pending_initial_ = false;
    apply_(initial_);
}

void EnvPresetSwitcher::select(int index) {
    // Deliberately no "already selected" early-out: picking the live preset replays it.
    apply_(index);
}

bool EnvPresetSwitcher::select_by_name(const std::string& name) {
    for (std::size_t i = 0; i < presets.size(); ++i) {
        if (presets[i].name == name) { select(static_cast<int>(i)); return true; }
    }
    return false;
}

void EnvPresetSwitcher::apply_(int index) {
    if (!scene || index < 0 || index >= static_cast<int>(presets.size())) return;
    selected_ = index;
    const EnvPreset& chosen = presets[static_cast<std::size_t>(index)];

    // Exactly one preset's subtree is live. Deactivating is what switches an EnvInfluence off:
    // the gather skips components whose object (or any ancestor) is inactive.
    coopa::scene::SceneObject* live = nullptr;
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const std::string& obj = presets[i].object;
        if (obj.empty()) continue;
        coopa::scene::SceneObject* o = scene->find_object(obj);
        if (!o) {
            std::cout << "[env] EnvPresetSwitcher: preset '" << presets[i].name
                      << "' names object '" << obj << "', which is not in the scene.\n";
            continue;
        }
        const bool chosen_obj = static_cast<int>(i) == index;
        o->set_active(chosen_obj);
        if (chosen_obj) live = o;
    }

    // Start it the first time (an object authored `active: false` has never been started), then
    // replay it, so switching always shows the effect from its beginning.
    if (live) {
        ensure_started_(*live);
        restart_subtree_(*live);
    }

    // AnimationSystem caches its animator list and only re-gathers when an Animator is created
    // or destroyed -- activating an object does not bump that. Without this nudge a preset's
    // clip reports is_playing() yet never advances a frame, because the system is still holding
    // the list it built while the preset was switched off. (The particle runner re-gathers every
    // execute, so it needs no equivalent.)
    if (auto* anim = dynamic_cast<coopa::anim::AnimationSystem*>(scene->find_system("Animation"))) {
        anim->refresh();
    }

    // The global layer moves with it, so a preset can be "the weather does this" rather than
    // only "a local source does this".
    if (!chosen.condition.empty()) {
        if (weather::WeatherSystem* w = weather::WeatherSystem::active()) {
            w->set_condition(chosen.condition, chosen.transition);
        }
    }


    on_selected.emit(index, chosen.name);
}

void EnvPresetSwitcher::ensure_started_(coopa::scene::SceneObject& root) {
    // Only once: a second start() would re-register rigidbodies and re-run every other
    // component's one-time setup. The replay below is what makes repeat switches visible.
    if (!started_.insert(&root).second) return;
    root.start();
}

void EnvPresetSwitcher::restart_subtree_(coopa::scene::SceneObject& root) {
    for (const auto& comp : root.components()) {
        coopa::scene::Component* c = comp.get();
        if (auto* a = dynamic_cast<coopa::anim::Animator*>(c)) {
            if (!a->current_state().empty()) a->play(a->current_state(), 0.0f);
        } else if (auto* ps = dynamic_cast<particles::ParticleSystem*>(c)) {
            ps->restart();
        } else if (auto* km = dynamic_cast<scene::KinematicMover*>(c)) {
            km->start();   // re-captures the seed transform, so the motion starts from here
        }
    }
    for (const auto& child : root.children()) restart_subtree_(*child);
}

/** @brief Registers "EnvPresetSwitcher". */
void register_env_preset_component() {
    using coopa::scene::SceneLoader;
    SceneLoader::register_component_parser("EnvPresetSwitcher",
        [](const fkyaml::node& node, coopa::scene::SceneObject& obj, const SceneLoader::ParseContext&) {
            auto* s = obj.add_component<EnvPresetSwitcher>();
            auto str = [](const fkyaml::node& n, const char* k, std::string& out) {
                if (n.contains(k) && n.at(k).is_string()) out = n.at(k).get_value<std::string>();
            };
            str(node, "start", s->start_preset);
            str(node, "title", s->title);
            str(node, "theme_ref", s->theme_ref);
            if (!node.contains("presets") || !node.at("presets").is_sequence()) return;
            for (const auto& pn : node.at("presets")) {
                EnvPreset p;
                str(pn, "name", p.name);
                str(pn, "label", p.label);
                str(pn, "object", p.object);
                str(pn, "condition", p.condition);
                str(pn, "blurb", p.blurb);
                if (pn.contains("transition")) {
                    const fkyaml::node& t = pn.at("transition");
                    if (t.is_float_number()) p.transition = static_cast<float>(t.get_value<double>());
                    else if (t.is_integer()) p.transition = static_cast<float>(t.get_value<int64_t>());
                }
                s->presets.push_back(std::move(p));
            }
        });
}

void EnvPresetSwitcher::draw(coopa::ui::imm::Context& ctx) {
    using coopa::ui::imm::Anchor;
    using coopa::ui::imm::Box;
    if (presets.empty()) return;

    const float row = ctx.style.row_height + 10.0f;
    const float gap = ctx.style.spacing;
    const float list_h = row * static_cast<float>(presets.size()) +
                         gap * static_cast<float>(presets.size() - 1);
    // The blurb is a sentence or two under the list, and it wraps -- so the panel reserves
    // several lines for it rather than a fixed strip that cuts the last one off.
    const float blurb_h = ctx.text.line_height(ctx.style.font_size) * 4.0f;
    const Box frame = tune().box(ctx, "panel", Anchor::MiddleLeft, {24.0f, 0.0f},
                                 {320.0f, list_h + blurb_h + 72.0f});
    const Box inner = title.empty() ? frame : ctx.panel_frame("presets", frame, title);

    std::vector<std::string> labels;
    labels.reserve(presets.size());
    for (const EnvPreset& p : presets) labels.push_back(p.label.empty() ? p.name : p.label);

    const int hit = ctx.menu_list("presets", {inner.x, inner.y, inner.w, list_h}, labels);
    if (hit >= 0) apply_(hit);

    // "You are here": the live preset gets the accent outline. A tint on one button in a list
    // of identical ones is the whole reason this is drawn rather than left to a theme.
    if (selected_ >= 0 && static_cast<std::size_t>(selected_) < presets.size()) {
        const Box live{inner.x, inner.y + static_cast<float>(selected_) * (row + gap), inner.w, row};
        ctx.outline_rounded(live, ctx.style.accent, -1.0f, 2.0f);

        const std::string& blurb = presets[static_cast<std::size_t>(selected_)].blurb;
        if (!blurb.empty()) {
            ctx.begin_region("blurb", {inner.x, inner.y + list_h + 12.0f, inner.w,
                                       std::max(0.0f, inner.bottom() - (inner.y + list_h + 12.0f))},
                             false);
            ctx.paragraph(blurb);
            ctx.end_region();
        }
    }
}

} // namespace env
} // namespace toy
