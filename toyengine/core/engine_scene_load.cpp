/**
 * @file engine_scene_load.cpp
 * @brief The async scene-load state machine and its transition layer.
 *
 * load_scene_async() starts it; update_scene_load_() advances it each frame through warm-up,
 * activation and the fade/loading-screen transition. The state itself is detail::SceneLoadState
 * (toyengine/core/scene_loading.h).
 *
 * A sibling translation unit of engine.cpp -- same Engine class, same header
 * (toyengine/core/engine.h), split by concern.
 */

#include <toyengine/core/engine.h>

// Includes engine.h no longer carries: these are implementation needs of this file
// (and of the other engine TUs that previously got them transitively).
#include <gfxcoopa/engine/loaders/mesh_loader.h>
#include <gfxcoopa/engine/loaders/skinned_mesh_source_loader.h>
#include <gfxcoopa/engine/loaders/texture_loader.h>
#include <gfxcoopa/engine/components/register.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <coopa/animation/animation_system.h>
#include <coopa/animation/animation_yaml.h>
#include <coopa/animation/ik_system.h>
#include <coopa/scene/systems/transform_system.h>
#include <physxcoopa/physx_yaml.h>
#include <physxcoopa/debug/debug_draw.h>
#include <toyengine/core/branding.h>
#include <toyengine/core/user_settings.h>
#include <toyengine/audio/audio_components.h>
#include <sfxcoopa/sfx_yaml.h>
#include <uicoopa/audio/audio_yaml.h>
#include <uicoopa/audio/sound_library.h>
#include <uicoopa/ui_yaml.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/free_mover.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/register.h>
#include <toyengine/scene/scene_link.h>
#include <toyengine/world/snow_system.h>
#include <toyengine/world/terrain_system.h>
#include <toyengine/water/water_system.h>
#include <toyengine/particles/particle_system_runner.h>
#include <toyengine/particles/particle_yaml.h>
#include <toyengine/weather/weather_reactor.h>
#include <toyengine/render/visibility.h>
#include <toyengine/save/register.h>
#include <toyengine/world/terrain_component.h>

#include <toyengine/core/boot_splash.h>

#include <toyengine/audio/audio_system.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/kinematic_controller.h>
#include <toyengine/scene/mount.h>
#include <toyengine/weather/wind_system.h>
#include <toyengine/scene/ragdoll.h>
#include <toyengine/scene/rider.h>
#include <toyengine/scene/skinned_mesh_renderer.h>
#include <toyengine/scene/destruction_system.h>
#include <toyengine/scene/terrain_digger.h>
#include <toyengine/scene/turntable.h>
#include <toyengine/world/heightmap_terrain_system.h>

#include <uicoopa/input/modal_context.h>
#include <uicoopa/render/icon_library.h>
#include <uicoopa/layout/canvas.h>

namespace toy {
namespace core {

SceneLoadHandle Engine::begin_scene_warmup(SceneLoadOptions options) {
    if (!scene_mgr_.has_scene()) return SceneLoadHandle();
    if (!options.warm_up || options.transition.kind == SceneTransition::Kind::None) {
        return SceneLoadHandle();
    }
    if (scene_load_ && !scene_load_->finished()) return SceneLoadHandle(scene_load_);
    // A load that already happened: the document, the build and the assets are in (a startup
    // scene loads synchronously behind the boot splash, and that half was never the problem).
    // What is left is the world the scene's systems stream once it starts simulating, so the
    // state is dropped straight into Warming with the cover already full.
    coopa::scene::Scene& active = scene_mgr_.get_active_scene();
    auto st = std::make_shared<detail::SceneLoadState>();
    auto path = scene_paths_.find(&active);
    st->path = path != scene_paths_.end() ? path->second : std::string();
    st->options = std::move(options);
    st->stage = SceneLoadStage::Warming;
    st->progress = 1.0f;
    st->warm_only = true;
    st->alpha = 1.0f;
    st->scene = &active;
    scene_load_ = st;
    transition_.set_fade(st->options.transition.color, 1.0f);
    add_overlay_layer(&transition_.scene(), 50, /*in_display_rect=*/true);
    if (st->options.transition.kind == SceneTransition::Kind::LoadingScreen &&
        !st->options.transition.loading_screen.empty()) {
        st->loading_screen_shown = true;
        if (!transition_.show_loading_screen(st->options.transition.loading_screen)) {
            std::cerr << "[toyengine] Loading screen '" << st->options.transition.loading_screen
                      << "' could not be shown\n";
        }
    }
    return SceneLoadHandle(st);
}

SceneLoadHandle Engine::load_scene_async(const std::string& path, SceneLoadOptions options) {
    if (scene_load_ && scene_load_->pending()) {
        std::cerr << "[toyengine] load_scene_async('" << path << "'): '" << scene_load_->path
                  << "' is still loading; ignoring the new request\n";
        return SceneLoadHandle(scene_load_);
    }
    if (scene_load_ && !scene_load_->finished()) finish_transition_();   // a previous fade-in still running
    auto st = std::make_shared<detail::SceneLoadState>();
    st->path = resolve_scene_path_(path);
    st->options = std::move(options);
    st->peak_pending = assets_.pending_load_count();
    st->read = coopa::scene::SceneLoader::read_document_async(st->path, &jobs_);
    scene_load_ = st;
    if (st->options.transition.kind != SceneTransition::Kind::None) {
        transition_.set_fade(st->options.transition.color, 0.0f);
        // Over the game and the host's UI inside the display rect; under the stats HUD (100).
        add_overlay_layer(&transition_.scene(), 50, /*in_display_rect=*/true);
    }
    return SceneLoadHandle(st);
}

void Engine::apply_warmup_budgets_(detail::SceneLoadState& st, bool warming) {
    if (st.scene == nullptr || warming == warm_budgets_applied_) return;
    // Nothing is on screen yet, so the per-frame slices that pace streaming during gameplay are
    // far too small here -- at the authored 1.5 ms a frame, a continent's view radius takes
    // hundreds of frames to come up. Raised on entering the warm-up and put back at the reveal,
    // so the scene file keeps holding the steady-state values and gameplay streaming is
    // untouched. Bounded, not lifted: the loading screen is drawn out of these same frames and
    // stops animating if one runs long.
    if (warming) {
        const float budget = std::max(0.1f, st.options.warm_up_budget_ms);
        warm_saved_upload_ms_.clear();
        for (toy::world::TerrainComponent* terrain :
             st.scene->get_components<toy::world::TerrainComponent>()) {
            warm_saved_upload_ms_.emplace_back(terrain, terrain->upload_budget_ms);
            terrain->upload_budget_ms = budget;
        }
        if (auto* water = dynamic_cast<water::WaterSystem*>(st.scene->find_system("Water"))) {
            warm_saved_water_ = water->settings();
            water::WaterSettings ws = warm_saved_water_;
            ws.bake_budget_ms = budget;
            ws.new_bakes_per_frame = std::max(ws.new_bakes_per_frame, 16);
            ws.bakes_per_frame = std::max(ws.bakes_per_frame, 8);
            water->set_settings(ws);
        }
        // The navmesh is paced by wall clock, not by a budget: a dirty tile waits
        // `rebuild_delay` before it is rebuilt. That spreads the work over gameplay frames,
        // which behind a loading screen only makes the screen last longer -- a 2 km island's
        // tiles are dirtied in waves as its chunk colliders land, and each wave would wait out
        // another quarter second. Rebuilt as they arrive instead.
        //
        // `source_scan_interval` is deliberately left alone: scanning every frame walks every
        // physics shape in the scene and keeps finding the colliders still streaming in, so
        // zeroing it makes the dirty set churn faster than it drains and the warm-up never ends.
        if (auto* nav = dynamic_cast<coopa::physx::system::NavSystem*>(
                st.scene->find_system("Navigation"))) {
            warm_saved_nav_delay_ = nav->settings().build.rebuild_delay;
            nav->set_rebuild_pacing(0.0f, 0);
            warm_saved_nav_ = true;
        }
        warm_budgets_applied_ = true;
        return;
    }
    // Restored by the pointer each value was taken from: a chunk's budget is a component field,
    // and a terrain destroyed mid-warm-up must not be written through.
    const auto live = st.scene->get_components<toy::world::TerrainComponent>();
    for (const auto& [terrain, saved] : warm_saved_upload_ms_) {
        if (std::find(live.begin(), live.end(), terrain) != live.end()) {
            terrain->upload_budget_ms = saved;
        }
    }
    warm_saved_upload_ms_.clear();
    if (auto* water = dynamic_cast<water::WaterSystem*>(st.scene->find_system("Water"))) {
        water->set_settings(warm_saved_water_);
    }
    if (warm_saved_nav_) {
        if (auto* nav = dynamic_cast<coopa::physx::system::NavSystem*>(
                st.scene->find_system("Navigation"))) {
            nav->set_rebuild_pacing(warm_saved_nav_delay_, 0);
        }
        warm_saved_nav_ = false;
    }
    warm_budgets_applied_ = false;
}

bool Engine::poll_scene_warmup_(detail::SceneLoadState& st) {
    if (st.scene == nullptr) return true;
    return fold_warmup_states(st, st.scene->warm_up_states());
}

void Engine::update_scene_load_(float dt) {
    detail::SceneLoadState& st = *scene_load_;
    ++st.frames;
    st.elapsed += dt;
    const SceneTransition& tr = st.options.transition;
    const bool covered_kind = tr.kind != SceneTransition::Kind::None;
    st.peak_pending = std::max(st.peak_pending, assets_.pending_load_count());

    try {
        if (st.stage == SceneLoadStage::ReadingDocument && st.read && st.read->ready()) {
            if (st.read->failed()) throw std::runtime_error(st.read->error());
            const fkyaml::node& doc = st.read->document();
            if (doc.contains("scene") && doc.at("scene").contains("settings")) st.settings = doc.at("scene").at("settings");
            st.builder = std::make_unique<coopa::scene::SceneLoader::Builder>(doc, st.path, coopa::scene::SceneLoader::LoadOptions{.start = false});
            st.read.reset();
            st.stage = SceneLoadStage::Building;
        } else if (st.stage == SceneLoadStage::Building) {
            if (st.builder->step(st.options.build_budget_ms)) st.stage = SceneLoadStage::LoadingAssets;
        }
    } catch (const std::exception& e) {
        std::cerr << "[toyengine] Async load of '" << st.path << "' failed: " << e.what() << "\n";
        st.fail(e.what());
        transition_.hide_loading_screen();   // fade back in over the old scene
    }
    if (st.stage == SceneLoadStage::LoadingAssets && assets_.pending_load_count() == 0) {
        st.stage = SceneLoadStage::WaitingToSwap;
    }

    // Progress: the document 5%, the build 65%, the assets 30%.
    switch (st.stage) {
        case SceneLoadStage::ReadingDocument: break;
        case SceneLoadStage::Building:
            st.raise_progress(0.05f + 0.65f * st.builder->progress());
            break;
        case SceneLoadStage::LoadingAssets: {
            const float peak = static_cast<float>(std::max<size_t>(st.peak_pending, 1));
            st.raise_progress(0.70f + 0.30f * (1.0f - static_cast<float>(assets_.pending_load_count()) / peak));
            break;
        }
        default: st.raise_progress(st.stage == SceneLoadStage::Failed ? st.progress : 1.0f); break;
    }

    // The cover: up over fade_out before activation, down over fade_in after (or after a failure).
    if (st.stage == SceneLoadStage::FadingIn || st.stage == SceneLoadStage::Failed) {
        st.alpha = tr.fade_in > 0.0f ? std::max(0.0f, st.alpha - dt / tr.fade_in) : 0.0f;
    } else if (covered_kind) {
        st.alpha = tr.fade_out > 0.0f ? std::min(1.0f, st.elapsed / tr.fade_out) : 1.0f;
    }
    const bool covered = !covered_kind || st.alpha >= 1.0f;

    const bool warming = st.stage == SceneLoadStage::Warming;
    // Advanced every frame, shown or not: a screen that appears once the cover is full then
    // finds the bar already where it belongs, rather than catching up from zero.
    st.display_progress = advance_load_bar(st.display_progress, load_bar_target(st), dt);
    if (tr.kind == SceneTransition::Kind::LoadingScreen && (st.pending() || warming)) {
        if (covered && !st.loading_screen_shown) {
            st.loading_screen_shown = true;
            if (!tr.loading_screen.empty() && !transition_.show_loading_screen(tr.loading_screen)) {
                std::cerr << "[toyengine] Loading screen '" << tr.loading_screen << "' could not be shown\n";
            }
        }
        transition_.update_loading_screen(st.display_progress,
                                          warming && !st.warm_status.empty()
                                              ? st.warm_status
                                              : scene_load_status_text(st.stage));
    }

    if (st.stage == SceneLoadStage::WaitingToSwap && covered && st.elapsed >= st.options.min_display_time) {
        activate_scene_load_(st);
    }

    if (st.stage == SceneLoadStage::Warming) {
        st.warm_elapsed += dt;
        ++st.warm_frames;
        apply_warmup_budgets_(st, /*warming=*/true);
        const bool timed_out =
            st.options.warm_up_timeout > 0.0f && st.warm_elapsed >= st.options.warm_up_timeout;
        if (poll_scene_warmup_(st) || timed_out) {
            if (timed_out) {
                std::cerr << "[toyengine] '" << st.path << "' was still warming after "
                          << st.options.warm_up_timeout << " s (" << st.warm_status
                          << "); showing it anyway\n";
            }
            apply_warmup_budgets_(st, /*warming=*/false);   // back to the authored budgets
            transition_.hide_loading_screen();
            st.stage = SceneLoadStage::FadingIn;
            std::cout << "[toyengine] '" << st.path << "' warmed in " << st.warm_elapsed
                      << " s (" << st.warm_frames << " frames); revealing\n";
        }
    }

    if (covered_kind) transition_.set_fade(tr.color, st.alpha);
    if (st.stage == SceneLoadStage::FadingIn && st.alpha <= 0.0f) st.stage = SceneLoadStage::Done;
    if (st.finished() && st.alpha <= 0.0f) finish_transition_();
}

void Engine::activate_scene_load_(detail::SceneLoadState& st) {
    ctx_.wait_idle();
    // The loading screen stays up through a warm-up; it is dropped where the cover starts to
    // lift, in update_scene_load_(), so the two never come apart.
    if (!st.options.warm_up || st.options.transition.kind == SceneTransition::Kind::None) {
        transition_.hide_loading_screen();
    }
    std::unique_ptr<coopa::scene::Scene> built = st.builder->take();   // unstarted
    st.builder.reset();
    apply_spawn_point_(*built, st.options.spawn_point);
    coopa::scene::Scene* previous_active = scene_mgr_.has_scene() ? &scene_mgr_.get_active_scene() : nullptr;
    if (!st.options.additive) {
        clear_scenes_();
        previous_active = nullptr;
    }
    coopa::scene::Scene* raw = scene_mgr_.add_scene(std::move(built));
    scene_settings_[raw] = st.settings;
    scene_paths_[raw] = st.path;
    // The same order as load_scene(): start (SceneLoader::load() starts while building),
    // then the per-scene systems, so components that add bodies in start() are seen by them.
    raw->start();
    prepare_scene_(*raw, /*drain_assets=*/false);
    // Additive: the previously active scene stays the active one, with its own settings.
    if (previous_active) {
        scene_mgr_.set_active_scene(previous_active);
        apply_scene_settings_(*previous_active);
    }
    if (st.options.transition.kind == SceneTransition::Kind::None) {
        st.stage = SceneLoadStage::Done;
    } else if (st.options.warm_up) {
        // Still covered: the scene simulates from here, and update_scene_load_() holds the cover
        // and the loading screen until its systems report warm (scene_loading.h, stage 5).
        st.stage = SceneLoadStage::Warming;
    } else {
        st.stage = SceneLoadStage::FadingIn;
    }
    st.complete(*raw);
}

void Engine::apply_spawn_point_(coopa::scene::Scene& scene, const std::string& spawn_point) {
    if (spawn_point.empty()) return;
    coopa::scene::SceneObject* spawn = scene.find_object(spawn_point);
    auto* player = scene::find_player_controller(scene);
    if (!spawn || !player || !player->owner) {
        std::cerr << "[toyengine] spawn_point '" << spawn_point << "': "
                  << (spawn ? "no CharacterController in the scene" : "no such object") << "\n";
        return;
    }
    auto* stc = spawn->get_transform();
    auto* ptc = player->owner->get_transform();
    if (!stc || !ptc) return;
    const glm::mat4 world = stc->get_world_matrix();
    coopa::util::Transform& t = ptc->transform();
    glm::mat4 parent_inv(1.0f);
    if (player->owner->parent() && player->owner->parent()->get_transform()) {
        parent_inv = glm::inverse(player->owner->parent()->get_transform()->get_world_matrix());
    }
    t.set_position(glm::vec3(parent_inv * world[3]));
    // Facing: the spawn point's yaw (the controller turns about +Z).
    const glm::vec3 fwd = glm::vec3(world[1]);
    if (glm::length(glm::vec2(fwd)) > 1e-4f) {
        glm::vec3 euler = t.rotation_degrees();
        euler.z = glm::degrees(std::atan2(-fwd.x, fwd.y));
        t.set_rotation(euler);
    }
}

void Engine::cancel_scene_load_(const std::string& why) {
    if (!scene_load_ || !scene_load_->pending()) return;
    scene_load_->fail("cancelled: " + why);
    finish_transition_();
}

void Engine::finish_transition_() {
    if (scene_load_ && !scene_load_->finished()) scene_load_->stage = SceneLoadStage::Done;
    if (transition_.has_scene()) {
        transition_.hide_loading_screen();
        transition_.set_fade(glm::vec3(0.0f), 0.0f);
        remove_overlay_layer(&transition_.scene());
    }
    if (scene_load_) scene_load_->alpha = 0.0f;
}

void Engine::consume_scene_link_requests_() {
    std::vector<scene::SceneLinkRequest> requests = scene::SceneLinkRequests::instance().take();
    for (const scene::SceneLinkRequest& r : requests) {
        const auto managed = scene_mgr_.scenes();
        if (std::find(managed.begin(), managed.end(), r.origin) == managed.end()) continue;
        if (options_.edit_mode) {
            std::cout << "[toyengine] SceneLink to '" << r.target_scene << "' ignored in an embedded engine (editor play mode)\n";
            continue;
        }
        SceneLoadOptions o;
        const auto kind = SceneTransition::parse_kind(r.transition);
        if (!kind) std::cerr << "[toyengine] SceneLink: unknown transition '" << r.transition << "' (fade, loading_screen, none); fading\n";
        switch (kind.value_or(SceneTransition::Kind::Fade)) {
            case SceneTransition::Kind::None: o.transition = SceneTransition::none(); break;
            case SceneTransition::Kind::Fade: o.transition = SceneTransition::fade(r.fade_time, r.color); break;
            case SceneTransition::Kind::LoadingScreen:
                o.transition = SceneTransition::loading_screen_ui(r.loading_screen, r.fade_time, r.color);
                break;
        }
        o.min_display_time = r.min_display_time;
        o.spawn_point = r.spawn_point;
        load_scene_async(r.target_scene, std::move(o));
    }
}

SceneLoadOptions Engine::scene_loading_options_(const fkyaml::node& settings) {
    SceneLoadOptions opts;
    // No `loading:` block means no cover: a scene says for itself whether it is worth warming,
    // so adding the stage changes nothing for the scenes (and the projects) that do not ask.
    opts.warm_up = false;
    opts.transition = SceneTransition::none();
    if (!settings.is_mapping() || !settings.contains("loading")) return opts;
    const fkyaml::node& node = settings.at("loading");
    if (!node.is_mapping()) return opts;

    std::string ui;
    if (node.contains("ui")) ui = node.at("ui").get_value<std::string>();
    SceneTransition::Kind kind =
        ui.empty() ? SceneTransition::Kind::Fade : SceneTransition::Kind::LoadingScreen;
    if (node.contains("transition")) {
        if (auto parsed = SceneTransition::parse_kind(node.at("transition").get_value<std::string>())) {
            kind = *parsed;
        } else {
            std::cerr << "[toyengine] scene settings: loading.transition must be none, fade or "
                         "loading_screen\n";
        }
    }
    opts.transition.kind = kind;
    opts.transition.loading_screen = ui;
    opts.warm_up = kind != SceneTransition::Kind::None;
    if (node.contains("fade_in")) opts.transition.fade_in = node.at("fade_in").get_value<float>();
    if (node.contains("fade_out")) opts.transition.fade_out = node.at("fade_out").get_value<float>();
    if (node.contains("warm_up")) opts.warm_up = node.at("warm_up").get_value<bool>();
    if (node.contains("warm_up_timeout")) {
        opts.warm_up_timeout = node.at("warm_up_timeout").get_value<float>();
    }
    if (node.contains("warm_up_budget_ms")) {
        opts.warm_up_budget_ms = node.at("warm_up_budget_ms").get_value<float>();
    }
    if (node.contains("min_display_time")) {
        opts.min_display_time = node.at("min_display_time").get_value<float>();
    }
    return opts;
}
} // namespace core
} // namespace toy
