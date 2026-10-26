#include <toyengine/env/env_preset.h>

#include <iostream>

#include <coopa/scene/scene.h>
#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <uicoopa/binding/ui_handle.h>
#include <uicoopa/widgets/button.h>
#include <uicoopa/builder/theme_scope.h>

#include <coopa/animation/animation_system.h>
#include <coopa/animation/animator.h>
#include <toyengine/particles/particle_system.h>
#include <toyengine/scene/kinematic_mover.h>
#include <toyengine/weather/weather_system.h>

namespace toy {
namespace env {

void EnvPresetSwitcher::start() {
    if (!scene || presets.empty()) return;

    // Buttons are addressed by their UI object's name on the scene event bus, so the panel can
    // be its own asset loaded anywhere in the scene.
    clicks_.clear();
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        const std::string& button = presets[static_cast<std::size_t>(i)].button;
        if (button.empty()) continue;
        clicks_.emplace_back(scene->events().on(button, "click",
            [this, i](const coopa::event::EventArgs&) { select(i); }));
    }

    initial_ = 0;
    if (!start_preset.empty()) {
        for (std::size_t i = 0; i < presets.size(); ++i) {
            if (presets[i].name == start_preset) { initial_ = static_cast<int>(i); break; }
        }
    }
    // Deactivate every preset subtree now so none of them is live on frame 0; the chosen one
    // is applied from update(), once the panel's buttons exist.
    for (const EnvPreset& p : presets) {
        if (p.object.empty()) continue;
        if (coopa::scene::SceneObject* o = scene->find_object(p.object)) o->set_active(false);
    }
    pending_initial_ = true;
}

void EnvPresetSwitcher::update(float) {
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

    // "You are here". uicoopa has no radio / ToggleGroup widget, so the live button is tinted
    // with its own theme `pressed` shade and the rest are restored to their authored colour --
    // no palette is hardcoded here. Every button stays INTERACTABLE on purpose: clicking the
    // live one replays its effect, which is the whole point of a replayable preset.
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const std::string& b = presets[i].button;
        if (b.empty()) continue;
        coopa::scene::SceneObject* o = scene->find_object(b);
        if (!o) continue;
        auto* btn = coopa::ui::UiHandle(o).find<coopa::ui::Button>(b);
        if (!btn) continue;
        auto base = button_base_.find(b);
        if (base == button_base_.end()) base = button_base_.emplace(b, btn->colors.normal).first;
        // The theme's PRIMARY role is the "this is the one" colour. `pressed` would be the
        // obvious guess and is wrong: in the stock theme it is DARKER than normal, so the live
        // button is indistinguishable from the rest.
        const coopa::ui::UITheme& theme = coopa::ui::theme_for(o);
        btn->colors.normal = (static_cast<int>(i) == index) ? theme.button_primary.normal : base->second;
        btn->colors.highlighted = (static_cast<int>(i) == index) ? theme.button_primary.hover
                                                                 : theme.button.hover;
    }

    if (!label_object.empty()) {
        if (coopa::scene::SceneObject* o = scene->find_object(label_object)) {
            coopa::ui::UiHandle(o).set_text(label_object, chosen.blurb);
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
            str(node, "label_object", s->label_object);
            if (!node.contains("presets") || !node.at("presets").is_sequence()) return;
            for (const auto& pn : node.at("presets")) {
                EnvPreset p;
                str(pn, "name", p.name);
                str(pn, "button", p.button);
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

} // namespace env
} // namespace toy
