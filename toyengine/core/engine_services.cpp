/**
 * @file engine_services.cpp
 * @brief Audio, saves and the debug overlay: bring-up and per-frame ticks.
 *
 * The subsystems Engine owns but whose logic is not the frame loop's.
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
#include <uicoopa/ui_resources.h>
#include <uicoopa/audio/sound_library.h>
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

#include <uicoopa/render/icon_library.h>
#include <uicoopa/layout/canvas.h>

namespace toy {
namespace core {

void Engine::init_audio_() {
    audio::AudioSystemOptions ao;
    ao.sample_rate = config_.audio.sample_rate;
    ao.open_device = config_.audio.enabled;
    // A run with no visible window (tests, headless captures, the editor's offscreen
    // engines) never takes over the sound card.
    ao.null_backend = config_.audio.device == "null" || !config_.window.visible;
    audio_ = std::make_unique<audio::AudioSystem>(ao);
    audio::AudioSystem::set_active(audio_.get());

    auto& sfx = coopa::sfx::SfxResources::instance();
    sfx.set_engine(&audio_->engine());
    sfx.set_search_roots(asset_roots());
    coopa::sfx::register_sfx_components();
    audio::register_audio_components();
#ifdef UICOOPA_HAS_AUDIO
    // No component parsers to register: an immediate screen plays its cues through the
    // Context's on_sound hook (toy::ui::ImmScreen), not through per-widget reactors.
    coopa::ui::UiAudio::set_active(audio_->ui());
    // UI sounds: the first asset root with a sounds/ manifest (a packaged build carries
    // uicoopa's defaults there), else uicoopa's own from source.
    std::string sounds_dir;
    std::error_code ec;
    for (const std::string& r : asset_roots()) {
        if (coopa::yaml::document_exists(std::filesystem::path(r) / "sounds" / "sounds.yaml")) { sounds_dir = r + "/sounds"; break; }
    }
    if (sounds_dir.empty() && !RuntimeLayout::current().packaged()) sounds_dir = std::string(PROJ_DIR) + "/uicoopa/assets/sounds";
    if (!sounds_dir.empty() && coopa::yaml::document_exists(std::filesystem::path(sounds_dir) / "sounds.yaml")) {
        coopa::ui::SoundLibrary::instance().set_search_dir(sounds_dir);
        coopa::ui::SoundLibrary::instance().load_manifest("sounds.yaml");
    }
#endif
    // Bus volumes: config.yaml's defaults, then whatever this player chose last time
    // (init_user_settings_() has already loaded the file).
    UserSettings& us = UserSettings::instance();
    const std::pair<const char*, float> buses[] = {
        {audio::k_bus_master, config_.audio.master}, {audio::k_bus_music, config_.audio.music},
        {audio::k_bus_sfx, config_.audio.sfx}, {audio::k_bus_ui, config_.audio.ui}};
    for (const auto& [bus, def] : buses) audio_->set_bus_volume(bus, us.get_float(audio::volume_setting_key(bus), def));
}

void Engine::init_saves_() {
    if (const auto enc = save::parse_save_encoding(config_.save.encode)) {
        saves_.set_encoding(*enc);
    } else {
        std::cerr << "[toyengine] Unknown save.encode '" << config_.save.encode << "' (expected auto, yaml or caml)\n";
    }
    save::SaveSystem::Host host;
    host.scene = [this]() -> coopa::scene::Scene* { return scene_mgr_.has_scene() ? &scene_mgr_.get_active_scene() : nullptr; };
    host.scene_path = [this]() { return scene_mgr_.has_scene() ? save_scene_ref_(scene_path(scene_mgr_.get_active_scene())) : std::string(); };
    host.load_scene = [this](const std::string& scene, std::function<void(coopa::scene::Scene&)> ready,
                             std::function<void(const std::string&)> failed) {
        if (options_.edit_mode || scene_loading()) return false;
        SceneLoadHandle h = load_scene_async(scene);
        h.on_complete(std::move(ready));
        h.on_failed(std::move(failed));
        return true;
    };
    saves_.set_host(std::move(host));
    save::SaveSystem::set_active(&saves_);
}

std::string Engine::save_scene_ref_(const std::string& path) const {
    if (path.empty()) return path;
    std::error_code ec;
    const std::filesystem::path rel = std::filesystem::relative(path, options_.project_root, ec);
    if (ec || rel.empty() || *rel.begin() == "..") return path;
    return rel.generic_string();
}

void Engine::update_saves_(float dt) {
    if (edit_mode_ || !scene_mgr_.has_scene()) return;
    saves_.tick(dt);
    const std::string& slot = config_.save.quick_slot;
    if (slot.empty() || options_.edit_mode || input_blocked_() || scene_loading()) return;
    if (input_.is_pressed("quick_save", ctx_.input())) {
        if (saves_.save(slot)) std::cout << "[toyengine] Saved '" << slot << "' (" << saves_.slot_path(slot).string() << ")\n";
    } else if (input_.is_pressed("quick_load", ctx_.input()) && saves_.has_slot(slot)) {
        if (saves_.load(slot)) std::cout << "[toyengine] Loading '" << slot << "'\n";
    }
}

void Engine::shutdown_audio_() {
    if (!audio_) return;
    UserSettings::instance().flush();
    audio_->stop_all();
    if (audio::AudioSystem::active() == audio_.get()) audio::AudioSystem::set_active(nullptr);
    auto& sfx = coopa::sfx::SfxResources::instance();
    if (sfx.engine() == &audio_->engine()) sfx.set_engine(nullptr);
#ifdef UICOOPA_HAS_AUDIO
    if (coopa::ui::UiAudio::active() == audio_->ui()) coopa::ui::UiAudio::set_active(nullptr);
#endif
}

void Engine::update_audio_(float dt) {
    if (!audio_) return;
    glm::mat4 cam_world;
    const glm::mat4* cam_ptr = nullptr;
    if (auto* cam = coopa::gfx::engine::components::CameraComponent::main()) {
        cam_world = glm::inverse(cam->get_view_matrix());
        cam_ptr = &cam_world;
    }
    audio_->update(dt, cam_ptr);
    // Persist a changed volume promptly (a player who quits by killing the app keeps it).
    if (UserSettings::instance().dirty() && (++settings_flush_frames_ % 120) == 0) UserSettings::instance().flush();
}

void Engine::save_screenshot(const std::string& path, bool low_res) {
    coopa::gfx::util::save_image_png(capture_image(low_res), path);
    if (low_res) {
        std::cout << "[toyengine] Saved " << path << " (" << pipeline_->render_width()
                  << "x" << pipeline_->render_height() << ")\n";
    } else {
        std::cout << "[toyengine] Saved " << path << " (display resolution)\n";
    }
}

void Engine::update_debug_overlay_() {
    if constexpr (!debug::k_overlay_compiled) return;
    if (!edit_mode_ && !input_blocked_() && input_.is_pressed("debug_overlay", ctx_.input())) {
        debug_overlay_.cycle();
    }
    if (!debug_overlay_.visible()) {
        if (debug_overlay_.has_scene() && !overlay_layers_.empty()) remove_overlay_layer(&debug_overlay_.scene());
        return;
    }
    coopa::scene::Scene& layer = debug_overlay_.scene(debug_overlay_font_());
    const bool registered = std::any_of(overlay_layers_.begin(), overlay_layers_.end(),
                                        [&layer](const OverlayLayer& l) { return l.scene == &layer; });
    // Order 100: over any later engine layer of lower order (a scene transition fades
    // beneath the stats).
    if (!registered) add_overlay_layer(&layer, 100, /*in_display_rect=*/true);
    // Wall-clock, not FIXED_DT: the HUD reports how fast frames really are.
    debug_overlay_.record_frame(ctx_.delta_time() * 1000.0f);
}

coopa::ui::Font* Engine::debug_overlay_font_() {
    if (debug_overlay_font_path_.empty()) {
        const std::string mono = std::string(PROJ_DIR) + "/uicoopa/assets/fonts/JetBrainsMono-Regular.ttf";
        if (std::filesystem::exists(mono)) debug_overlay_font_path_ = mono;
        for (const std::string& root : asset_roots()) {
            if (!debug_overlay_font_path_.empty()) break;
            const std::filesystem::path p = std::filesystem::path(root) / "fonts" / "inter_regular.ttf";
            if (std::filesystem::exists(p)) debug_overlay_font_path_ = p.string();
        }
        if (debug_overlay_font_path_.empty()) return nullptr;
    }
    coopa::ui::Font* const before = coopa::ui::FontDefaults::font;
    coopa::ui::Font* font = coopa::ui::UIResourceCache::instance().font_for_path(debug_overlay_font_path_);
    if (!before && coopa::ui::FontDefaults::font == font) coopa::ui::FontDefaults::font = nullptr;
    return font;
}

void Engine::collect_debug_stats_() {
    debug::OverlayStats& st = debug_overlay_.stats();
    st = debug::OverlayStats{};
    if (const render::FrameProfile* prof = active_profile_()) {
        if (const render::FrameProfile::Row* row = prof->latest()) st.profile = *row;
    }
    st.render_width  = pipeline_->render_width();
    st.render_height = pipeline_->render_height();
    const auto& draws = pipeline_->last_frame_stats();
    st.draws             = draws.camera_draws;
    st.instances         = draws.camera_instances;
    st.triangles         = draws.camera_triangles;
    st.shadow_draws      = draws.shadow_draws;
    st.dir_levels_rendered = draws.dir_levels_rendered;
    st.dir_list_cache_hits = draws.dir_list_cache_hits;
    st.cull_items_tested   = draws.cull_items_tested;
    st.shadow_triangles  = draws.shadow_triangles;
    st.renderers         = draws.renderers;
    st.renderers_visible = draws.camera_visible;
    st.pending_assets    = assets_.pending_load_count();
    for (const auto& heap : ctx_.allocator().heap_budgets()) {
        st.heaps.push_back(debug::OverlayStats::Heap{heap.usage, heap.budget, heap.device_local});
    }
    if (!scene_mgr_.has_scene()) return;
    coopa::scene::Scene& scene = scene_mgr_.get_active_scene();

    std::function<void(const coopa::scene::SceneObject&)> count = [&](const coopa::scene::SceneObject& o) {
        ++st.objects;
        st.components += o.components().size();
        for (const auto& c : o.children()) count(*c);
    };
    for (const auto& root : scene.root_objects()) count(*root);
    if (auto* ps = dynamic_cast<particles::ParticleSimulationSystem*>(scene.find_system("Particles"))) {
        st.particles = ps->total_particles();
    }
    if (auto* phys = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"))) {
        st.has_physics = true;
        const auto& world = phys->world();
        world.for_each_body([&](coopa::physx::dynamics::BodyId, const coopa::physx::dynamics::Body& b) {
            ++st.bodies;
            if (b.awake && b.type == coopa::physx::dynamics::BodyType::Dynamic) ++st.bodies_awake;
        });
        st.contacts   = world.manifolds().size();
        st.physics_ms = phys->last_step_ms();
    }
    if (auto* nav = coopa::physx::system::find_nav_system(scene)) {
        st.has_nav    = true;
        st.nav_agents = nav->agent_count();
        st.nav_ms     = nav->stats().total_ms;
        st.nav_paths  = nav->stats().paths_planned;
    }
}

void Engine::gather_debug_lines_(coopa::scene::Scene& scene) {
    std::vector<render::DebugLine>& out = pipeline_->debug_lines();
    out.clear();
    // An embedding host (the editor's grid, gizmos, wireframe) appends after this, from
    // FrameHooks::pre_render -- which runs after this clear, so it always wins the frame.
    coopa::physx::debug::DebugDraw draw;
    // Navigation's overlay is opt-in per scene (`navigation.debug_draw`) and shows in every
    // debug_view: it is how a nav scene is read at all (walkable outline, paths, flow arrows).
    if (auto* nav = coopa::physx::system::find_nav_system(scene)) {
        nav->debug_draw(draw, nav->settings().debug_draw);
    }
    auto* sys = dynamic_cast<coopa::physx::system::PhysicsSystem*>(scene.find_system("Physics"));
    if (sys && render::parse_debug_view(config_.render.debug_view) == render::DebugView::Lines) {
        sys->world().debug_draw(draw, config_.physics.debug_draw);
    }
    out.reserve(draw.lines.size());
    for (const auto& line : draw.lines) {
        out.push_back(render::DebugLine{line.a, line.b, render::pack_gpu_color(line.color)});
    }
}

std::string Engine::profile_path_from_env_() {
    const char* v = debug_env("PROFILE");
    if (!v || !*v || std::string(v) == "0") return {};
    const std::string s(v);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".csv") == 0) return s;
    return "output/profile.csv";
}
} // namespace core
} // namespace toy
