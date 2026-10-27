/**
 * @file engine.cpp
 * @brief Engine's construction, the frame loop (tick/run) and scene management.
 *
 * Split across sibling translation units, all against toyengine/core/engine.h:
 *
 * | File | Holds |
 * |---|---|
 * | engine.cpp             | construction, run()/tick(), scenes, input, capture, config rebuilds |
 * | engine_render_sync.cpp | the sync_*_render_state_ family: scene state -> render config |
 * | engine_scene_load.cpp  | the async scene-load state machine and its transition layer |
 * | engine_services.cpp    | audio, saves and the debug overlay |
 *
 * Member declaration order in the header's private data block is destruction-order-critical
 * and is deliberately untouched by the split.
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
#include <physxcoopa/components/vehicle.h>
#include <physxcoopa/system/vehicle_system.h>
#include <toyengine/core/branding.h>
#include <toyengine/core/user_settings.h>
#include <toyengine/ui/ui_panel.h>
#include <toyengine/audio/audio_components.h>
#include <sfxcoopa/sfx_yaml.h>
#include <uicoopa/immediate/imm_canvas.h>
#include <uicoopa/text/font_defaults.h>
#include <uicoopa/ui_resources.h>
#include <uicoopa/audio/sound_library.h>
#include <toyengine/scene/camera_controller.h>
#include <toyengine/scene/free_mover.h>
#include <toyengine/scene/kinematic_control_system.h>
#include <toyengine/scene/register.h>
#include <toyengine/scene/scene_link.h>
#include <toyengine/world/snow_system.h>
#include <toyengine/mapgen/map_generator_component.h>
#include <toyengine/world/terrain_system.h>
#include <toyengine/water/water_system.h>
#include <toyengine/particles/particle_system_runner.h>
#include <toyengine/env/env_system.h>
#include <toyengine/fire/fire_system.h>
#include <toyengine/particles/particle_yaml.h>
#include <toyengine/instance/instance_painter.h>
#include <toyengine/instance/instance_painter_system.h>
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
#include <toyengine/scene/view_blocker_system.h>
#include <toyengine/world/heightmap_terrain_system.h>

#include <uicoopa/render/icon_library.h>
#include <uicoopa/layout/canvas.h>

namespace toy {
namespace core {

Engine::Engine(AppConfig config, EngineOptions options)
    : options_(normalize_options_(std::move(options))),
      config_(std::move(config)),
      jobs_(config_.jobs.worker_threads ? config_.jobs.worker_threads
                                         : std::thread::hardware_concurrency()),
      ctx_(make_context_config_(config_)),
      boot_splash_(make_boot_splash_(ctx_, config_, options_)),
      pipeline_(std::make_unique<render::ToyRenderPipeline>(
          ctx_.device(), ctx_.allocator(), ctx_.swapchain(), ctx_.render_pass(),
          ctx_.command_pool(), make_render_config_(config_, options_.project_root))),
      assets_(&jobs_)
{
    // Read once here rather than in the initializer list -- these are declared after
    // assets_/scene_mgr_/input_, and initializing them there regardless of list order
    // (member init always follows DECLARATION order) would trip -Wreorder for no benefit,
    // since neither env read depends on any other member.
    fixed_dt_       = fixed_dt_from_env_();
    capture_frames_ = capture_frames_from_env_();
    capture_ring_   = capture_ring_from_env_();
    no_input_       = no_input_from_env_();

    bind_default_input_();

    scene_mgr_.set_job_engine(&jobs_);
    pipeline_->set_job_engine(&jobs_);
    pipeline_->set_parallel_threshold(config_.jobs.parallel_threshold);

    // Profiling mode (PROFILE env var): per-frame CPU/GPU timings to a CSV + an exit summary.
    if (const std::string path = profile_path_from_env_(); !path.empty()) {
        profile_ = std::make_unique<render::FrameProfile>(path);
        if (profile_->ok()) {
            pipeline_->set_profiler(profile_.get());
            std::cout << "[profile] Profiling mode: per-frame timings -> " << path << "\n";
        } else {
            std::cerr << "[profile] Could not open '" << path << "' for writing; profiling off\n";
            profile_.reset();
        }
    }

    // The stats overlay's startup mode (config.yaml `debug.overlay`); F3 cycles it later.
    if (const auto mode = debug::parse_overlay_mode(config_.debug.overlay)) {
        debug_overlay_.set_mode(*mode);
    } else {
        std::cerr << "[toyengine] Unknown debug.overlay '" << config_.debug.overlay
                  << "' (expected off, fps or full); the overlay starts off\n";
    }

    edit_mode_ = options_.edit_mode;
    boot_progress("Loading engine assets...", 0.25f);
    // The project's assets first, then the engine checkout's as the fallback layer (shared
    // meshes, materials, fonts, UI themes); the same directory when building toyengine itself.
    for (const std::string& root : asset_roots()) assets_.add_search_root(root);
    // `prefab: objects/crate` (object assets) resolves against the same roots.
    coopa::scene::SceneLoader::set_search_roots(asset_roots());
    assets_.register_loader<coopa::gfx::engine::data::Mesh>(
        std::make_unique<coopa::gfx::engine::loaders::MeshLoader>(ctx_.device(), ctx_.allocator(), ctx_.command_pool()));
    // Pure-CPU bind-pose data for SkinnedMeshRenderer (toy::scene) -- no GPU handles, unlike
    // the Mesh loader above; see skinned_mesh_source.h's file doc for why toyengine CPU-skins
    // instead of uploading bone matrices to a shader.
    assets_.register_loader<coopa::gfx::engine::data::SkinnedMeshSource>(
        std::make_unique<coopa::gfx::engine::loaders::SkinnedMeshSourceLoader>());
    // Hard-edged texture sampling. texel_aa (the default): LINEAR + clamp-to-edge
    // (SamplerDesc::pixel_art_smooth()), paired with gfx_texel_aa_uv() in gbuffer.frag --
    // texel interiors render flat and hard-edged exactly like point sampling, but texel
    // boundaries blend over one screen pixel, so a moving camera glides them sub-pixel
    // instead of snapping them to the pixel grid (the snap reads as full-surface shimmer
    // on magnified texels). texel_aa: false restores raw NEAREST point sampling for A/B.
    assets_.register_loader<coopa::gfx::engine::data::Texture>(
        std::make_unique<coopa::gfx::engine::loaders::TextureLoader>(
            ctx_.device(), ctx_.allocator(), ctx_.command_pool(),
            config_.render.texel_aa ? coopa::gfx::SamplerDesc::pixel_art_smooth()
                                    : coopa::gfx::SamplerDesc::pixel_art()));
    // Authored UI panels (toyengine/ui/ui_panel.h), so a PanelScreen gets the asset system's
    // caching -- N screens on one file parse it once -- and hot reload while the editor is
    // previewing it.
    toy::ui::register_panel_loader(assets_);
    toy::ui::set_panel_assets(&assets_);
    coopa::gfx::engine::components::register_render_components(ctx_.device(), ctx_.allocator(), ctx_.command_pool(), assets_);
    // The GPU-aware overload (a strict superset of the argument-free one): ClothRenderer
    // allocates its own dynamic vertex buffers and publishes the result as a runtime asset,
    // so it needs the same device/allocator/assets capture register_render_components() takes.
    scene::register_scene_components(ctx_.device(), ctx_.allocator(), assets_,
                                      ctx_.frames_in_flight());
    coopa::physx::register_physics_components(assets_, config_.physics);
    // "ParticleSystem" + "LightFlicker" (toyengine/particles/): emitter meshes load CPU-side,
    // mesh-mode render meshes and sprite textures through the same loaders as MeshRenderer.
    particles::register_particle_components(assets_);
    // "WeatherReactor" (toyengine/weather/): objects switching with the time of day / weather.
    weather::register_weather_components();
    // "EnvInfluence" / "EnvPulseEmitter" (toyengine/env/): local environmental influences that
    // add to -- or override -- the global weather: a rotor's downwash, a vent, a sword swing.
    env::register_env_components();
    // "Burnable" / "FireIgniter" (toyengine/fire/): an object that catches fire at a point and
    // burns across its own surface, and the click that lights it. A purely visual fire is a
    // particle prefab (objects/props/campfire), not a component.
    fire::register_fire_components();
    // "InstancePainter" (toyengine/instance/): brush-placed instances of object assets, drawn
    // as instanced meshes. Its records live in a `.instances` file beside the scene, not here.
    instance::register_instance_components(assets_);
    // "SnowDeformer" (toyengine/world/snow_system.h): objects pressing trails into deep snow.
    world::register_snow_components();
    // "SaveId" (a stable identity in saves) and the demo "SaveDemo" (toyengine/save/).
    save::register_save_components();
    // The GPU overload, so font/sprite paths in scene YAML load on demand and
    // FontDefaults::resolve_font is wired for themes. Must precede load_scene(), like
    // every other parser registration above. Its captured device/allocator references
    // are released by the SceneLoader::clear_component_parsers() already in ~Engine().
    // The font cache every UI loads through, and the hook a theme's `font:` resolves by.
    coopa::ui::UIResourceCache::instance().configure(ctx_.device(), ctx_.allocator(),
                                                     ctx_.command_pool());
    coopa::ui::FontDefaults::resolve_font = [](const std::string& path) {
        return coopa::ui::UIResourceCache::instance().font_for_path(path);
    };
    // Where a theme's font paths are looked up when they are not beside the theme file: the
    // same roots every other asset ref resolves against, so a project's own theme can say
    // `fonts/inter_regular.ttf` and get the engine's shared face. See FontDefaults::search_dirs.
    coopa::ui::FontDefaults::search_dirs = asset_roots();
    init_ui_icons_();
    // Registers the AnimationClip asset loader and the "Animator" component parser --
    // blendy exports skeletal/object animation as baked Transform keyframe tracks (there is
    // no vertex skinning in this engine), referenced from a scene's `Animator` component.
    // Must precede load_scene(), like every other parser registration above.
    coopa::anim::register_animation_components(assets_);
    init_user_settings_();
    init_audio_();
    init_saves_();
    // Project modules (TOY_MODULE in a project's src/, see module.h) last, so they can
    // build on -- or replace -- any parser registered above.
    for (const Module& m : modules()) {
        if (m.on_engine_init) m.on_engine_init(*this);
    }

    if (options_.load_default_scene) {
        boot_progress("Loading scene...", 0.4f);
        load_scene(scene_path_from_env_(config_.scene.default_scene));
        // The scene is loaded but its streamed world is not built yet; if it asked to be warmed,
        // hand the boot splash over to a cover that stays up until it is (begin_scene_warmup()).
        if (scene_mgr_.has_scene()) {
            begin_scene_warmup(scene_loading_options_(scene_settings_[&scene_mgr_.get_active_scene()]));
        }
    }
    read_cursor_pos_override_();
}

coopa::scene::Scene& Engine::load_scene(const std::string& path) {
    ctx_.wait_idle();
    cancel_scene_load_("superseded by load_scene()");
    const std::string resolved = resolve_scene_path_(path);
    scene_mgr_.load_scene(resolved);
    scene_settings_[&scene_mgr_.get_active_scene()] = read_scene_settings_(resolved);
    scene_paths_[&scene_mgr_.get_active_scene()] = resolved;
    prepare_scene_(scene_mgr_.get_active_scene());
    return scene_mgr_.get_active_scene();
}

coopa::scene::Scene& Engine::set_scene(coopa::scene::Scene&& scene, const fkyaml::node& settings) {
    ctx_.wait_idle();
    cancel_scene_load_("superseded by set_scene()");
    clear_scenes_();
    coopa::scene::Scene* raw = scene_mgr_.add_scene(std::make_unique<coopa::scene::Scene>(std::move(scene)));
    scene_settings_[raw] = settings;
    prepare_scene_(*raw);
    return *raw;
}

coopa::scene::Scene& Engine::push_scene(coopa::scene::Scene&& scene, bool simulating,
                                const fkyaml::node& settings) {
    ctx_.wait_idle();
    if (scene_mgr_.has_scene()) scene_mgr_.set_scene_active(&scene_mgr_.get_active_scene(), false);
    coopa::scene::Scene* raw = scene_mgr_.add_scene(std::make_unique<coopa::scene::Scene>(std::move(scene)));
    scene_settings_[raw] = settings;
    scene_mgr_.set_active_scene(raw);
    prepare_scene_(*raw);
    raw->set_simulating(simulating);
    return *raw;
}

void Engine::remove_scene(coopa::scene::Scene* scene) {
    ctx_.wait_idle();
    scene_mgr_.remove_scene(scene);
    scene_settings_.erase(scene);
    scene_paths_.erase(scene);
    if (scene_mgr_.has_scene()) apply_scene_settings_(scene_mgr_.get_active_scene());
}

void Engine::activate_scene(coopa::scene::Scene* scene) {
    for (coopa::scene::Scene* s : scene_mgr_.scenes()) scene_mgr_.set_scene_active(s, s == scene);
    scene_mgr_.set_active_scene(scene);
    apply_scene_settings_(*scene);
}

void Engine::set_config_source(const fkyaml::node& config_yaml, std::function<void(AppConfig&)> adjust) {
    config_.source = config_yaml.is_mapping() ? config_yaml : fkyaml::node::mapping();
    config_adjust_ = std::move(adjust);
    source_driven_ = true;
    if (scene_mgr_.has_scene()) apply_scene_settings_(scene_mgr_.get_active_scene(), kLiveRebuildDebounceFrames);
}

void Engine::set_scene_settings(coopa::scene::Scene& scene, const fkyaml::node& settings) {
    scene_settings_[&scene] = settings;
    if (scene_mgr_.has_scene() && &scene_mgr_.get_active_scene() == &scene)
        apply_scene_settings_(scene, kLiveRebuildDebounceFrames);
    if (weather::WeatherSystem* w = weather::find(scene)) w->set_settings(weather::parse_settings(weather_node_(settings)));
}

std::string Engine::scene_path(const coopa::scene::Scene& scene) const {
    auto it = scene_paths_.find(&scene);
    return it != scene_paths_.end() ? it->second : std::string();
}

AppConfig Engine::scene_config(const coopa::scene::Scene& scene) const {
    auto it = scene_settings_.find(&scene);
    const fkyaml::node none;
    const fkyaml::node& settings = it != scene_settings_.end() ? it->second : none;
    if (!source_driven_) return config_.with_scene_settings(settings);
    AppConfig derived = AppConfig::from_node(config_.source).with_scene_settings(settings);
    derived.window = config_.window;   // what the adjust hook sizes against
    if (config_adjust_) config_adjust_(derived);
    AppConfig out = config_;
    out.render = derived.render;
    out.physics = derived.physics;
    out.navigation = derived.navigation;
    return out;
}

void Engine::set_edit_mode(bool edit) {
    edit_mode_ = edit;
    if (scene_mgr_.has_scene()) scene_mgr_.get_active_scene().set_simulating(!edit);
    // Edit mode never holds the pointer: hand back a cursor a game may have captured.
    if (edit && ctx_.input().cursor_mode() != coopa::input::CursorMode::Normal) {
        ctx_.input().set_cursor_mode(coopa::input::CursorMode::Normal);
    }
}

void Engine::set_game_input_focus(bool focused) {
    game_focused_ = focused;
    if (focused) {
        // Edit mode never captures (see set_edit_mode()): focus is only recorded for when
        // the game runs again.
        if (edit_mode_ || no_input_ || !config_.window.visible || !scene_mgr_.has_scene()) return;
        auto* cc = scene_mgr_.get_active_scene().find_first_component<scene::CameraController>();
        if (cc && cc->capture_cursor) ctx_.input().set_cursor_mode(coopa::input::CursorMode::Disabled);
    } else if (ctx_.input().cursor_mode() != coopa::input::CursorMode::Normal) {
        ctx_.input().set_cursor_mode(coopa::input::CursorMode::Normal);
    }
}

void Engine::set_overlay_scene(coopa::scene::Scene* scene) {
    overlay_scene_ = scene;
    pipeline_->set_overlay_scene(scene);
}

void Engine::add_overlay_layer(coopa::scene::Scene* scene, int order, bool in_display_rect, bool input) {
    if (!scene) return;
    std::erase_if(overlay_layers_, [scene](const OverlayLayer& l) { return l.scene == scene; });
    auto at = std::find_if(overlay_layers_.begin(), overlay_layers_.end(),
                           [order](const OverlayLayer& l) { return l.order > order; });
    overlay_layers_.insert(at, OverlayLayer{scene, order, in_display_rect, input});
    sync_overlay_layers_();
}

void Engine::remove_overlay_layer(coopa::scene::Scene* scene) {
    if (std::erase_if(overlay_layers_, [scene](const OverlayLayer& l) { return l.scene == scene; })) {
        sync_overlay_layers_();
    }
}

std::vector<coopa::scene::Scene*> Engine::overlay_layers() const {
    std::vector<coopa::scene::Scene*> out;
    for (const OverlayLayer& l : overlay_layers_) out.push_back(l.scene);
    return out;
}

render::LetterboxRect Engine::display_rect() const {
    return pipeline_->display_rect_for(const_cast<coopa::gfx::app::Context&>(ctx_).swapchain().extent().width,
                                     const_cast<coopa::gfx::app::Context&>(ctx_).swapchain().extent().height);
}

bool Engine::viewport_ray(const glm::vec2& window_px, glm::vec3& out_origin, glm::vec3& out_dir) const {
    auto* cam = coopa::gfx::engine::components::CameraComponent::main();
    if (!cam) return false;
    const uint32_t rw = pipeline_->render_width();
    const uint32_t rh = pipeline_->render_height();
    if (rw == 0 || rh == 0) return false;
    const render::LetterboxRect box = display_rect();
    if (box.w == 0 || box.h == 0) return false;
    const glm::vec2 local = window_px - glm::vec2(box.x, box.y);
    if (local.x < 0 || local.y < 0 || local.x > box.w || local.y > box.h) return false;
    const float aspect = static_cast<float>(rw) / static_cast<float>(rh);
    const glm::mat4 inv_vp = glm::inverse(cam->get_projection_matrix(aspect) * cam->get_view_matrix());
    const glm::vec2 ndc(2.0f * local.x / static_cast<float>(box.w) - 1.0f,
                        1.0f - 2.0f * local.y / static_cast<float>(box.h));
    glm::vec4 near_h = inv_vp * glm::vec4(ndc, 0.0f, 1.0f);
    glm::vec4 far_h  = inv_vp * glm::vec4(ndc, 1.0f, 1.0f);
    if (std::abs(near_h.w) < 1e-9f || std::abs(far_h.w) < 1e-9f) return false;
    out_origin = glm::vec3(near_h) / near_h.w;
    out_dir = glm::vec3(far_h) / far_h.w - out_origin;
    const float len = glm::length(out_dir);
    if (len < 1e-12f) return false;
    out_dir /= len;
    return true;
}

bool Engine::world_to_window(const glm::vec3& world, glm::vec2& out_px) const {
    auto* cam = coopa::gfx::engine::components::CameraComponent::main();
    if (!cam) return false;
    const uint32_t rw = pipeline_->render_width();
    const uint32_t rh = pipeline_->render_height();
    const render::LetterboxRect box = display_rect();
    if (rw == 0 || rh == 0 || box.w == 0) return false;
    const float aspect = static_cast<float>(rw) / static_cast<float>(rh);
    const glm::vec4 clip = cam->get_projection_matrix(aspect) * cam->get_view_matrix() * glm::vec4(world, 1.0f);
    if (clip.w <= 1e-6f) return false;
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    out_px = glm::vec2(box.x + (ndc.x * 0.5f + 0.5f) * box.w,
                       box.y + (0.5f - ndc.y * 0.5f) * box.h);
    return true;
}

std::vector<std::string> Engine::asset_roots() const {
    std::vector<std::string> roots{(options_.project_root / "assets").string()};
    const std::filesystem::path& engine_assets = RuntimeLayout::current().engine_assets;
    std::error_code ec;
    if (!engine_assets.empty() &&
        !std::filesystem::equivalent(options_.project_root / "assets", engine_assets, ec)) {
        roots.push_back(engine_assets.string());
    }
    return roots;
}

EngineOptions Engine::normalize_options_(EngineOptions o) {
    prepare_runtime_environment();
    if (o.project_root.empty()) o.project_root = default_project_root();
    return o;
}

void Engine::init_ui_icons_() {
    // The themed widgets draw real icons where they can -- a Toggle's checkmark, a ComboBox's
    // arrow, a Scrollbar's step buttons, a SpinBox's +/- -- and fall back to a text glyph or to
    // nothing per icon when IconLibrary is empty (see UITheme::IconStyle). Publishing the
    // shipped sheet once here is what turns every one of those fallbacks into the real thing.
    coopa::ui::IconLibrary::instance().configure(assets_, ctx_.device(), ctx_.allocator(),
                                                 ctx_.command_pool());
    // The sheet ships with uicoopa rather than with a game, so it is reached the same way its
    // sounds are: a project's own assets/icons first (a game may replace the set), then the
    // library's own copy beside the source. A packaged build carries it in assets/ already.
    for (const std::string& root : asset_roots()) {
        if (coopa::yaml::document_exists(std::filesystem::path(root) / "icons" / "icons.yaml")) {
            if (coopa::ui::IconLibrary::instance().add_sheet(assets_, "icons/icons.yaml")) return;
            break;
        }
    }
    if (RuntimeLayout::current().packaged()) return;
    assets_.add_search_root(std::string(PROJ_DIR) + "/uicoopa/assets");
    coopa::ui::IconLibrary::instance().add_sheet(assets_, "icons/icons.yaml");
}

void Engine::init_user_settings_() {
    UserSettings& us = UserSettings::instance();
    us.load();
    // Everything else this file layers over config.yaml defaults -- bus volumes, and whatever a
    // game's own settings screen stores -- reads the same document; this only has to run first.
    set_ui_scale(us.get_float(k_ui_scale_setting, 1.0f));
    // Startup-only: the present mode is chosen when the swapchain is built, so this one takes
    // effect on the next launch. It is read here so the key a settings panel writes is not
    // simply ignored (engine_setting_keys() says which keys have a consumer at all).
    config_.window.vsync = us.get_bool("display.vsync", config_.window.vsync);

    // Applied the moment any writer changes one, so a settings slider is audible straight
    // away rather than next launch. Scoped to this Engine: the test suite builds and destroys
    // many against the one process-wide UserSettings.
    settings_conn_ = us.on_changed.connect_scoped([this](const std::string& key) {
        UserSettings& cur = UserSettings::instance();
        if (key == k_ui_scale_setting) {
            set_ui_scale(cur.get_float(key, 1.0f));
            return;
        }
        if (!audio_) return;
        for (const char* bus : {audio::k_bus_master, audio::k_bus_music, audio::k_bus_sfx,
                                audio::k_bus_ui}) {
            if (key == audio::volume_setting_key(bus)) {
                audio_->set_bus_volume(bus, cur.get_float(key, 1.0f));
                return;
            }
        }
    });
}

void Engine::clear_scenes_() {
    // Music carries over a scene change (the next scene's MusicPlaylist picks it up or replaces it).
    if (audio_) audio_->stop_oneshots();
    for (coopa::scene::Scene* s : scene_mgr_.scenes()) scene_mgr_.remove_scene(s);
    scene_settings_.clear();
    scene_paths_.clear();
}

fkyaml::node Engine::read_scene_settings_(const std::string& path) {
    try {
        const fkyaml::node doc = coopa::yaml::load_document(coopa::yaml::resolve_variant(path));
        if (doc.contains("scene") && doc.at("scene").contains("settings")) return doc.at("scene").at("settings");
    } catch (const std::exception&) {}
    return fkyaml::node();
}

AppConfig Engine::apply_scene_settings_(const coopa::scene::Scene& scene, int rebuild_delay) {
    // The weather's writes are not config: put the config's own values back first, so they
    // are what gets layered or kept (the weather re-captures and re-applies next frame).
    restore_weather_atmosphere_();
    restore_sky_colours_();
    const AppConfig eff = scene_config(scene);
    auto it = scene_settings_.find(&scene);
    const bool overrides = it != scene_settings_.end() && AppConfig::has_scene_overrides(it->second);
    if (overrides || overrides_applied_ || source_driven_) {
        render::ToyRenderConfig next = make_render_config_(eff, options_.project_root);
        const render::ToyRenderConfig& live = pipeline_->render_config();
        next.shaders = live.shaders;
        next.surface_shaders = live.surface_shaders;
        next.debug_view = live.debug_view;
        next.fill_aspect = live.fill_aspect;   // the engine's, from the display region -- see update_fill_extent_()
        // Fields fixed at construction (a feature switch, a target size) need a new
        // pipeline: queue one for the top of the next tick. Everything else applies now.
        // Compared with what the config asked for last time, not with the live pipeline:
        // only fields the config / scene actually changed rebuild, and a runtime tweak of
        // render_config() (a test, game code) is never undone.
        if (!derived_render_) derived_render_ = source_render_config_();
        if (render::ToyRenderPipeline::needs_rebuild(*derived_render_, next)) queue_rebuild_(*derived_render_, next, rebuild_delay);
        derived_render_ = next;
        pipeline_->apply_live_config(next, /*warn_ignored=*/false);
    }
    overrides_applied_ = overrides;
    return eff;
}

void Engine::prepare_scene_(coopa::scene::Scene& scene, bool drain_assets) {
    // The scene's own settings first: render (live) before the water system reads
    // water_quality below, physics for install_physics_system().
    const AppConfig scene_cfg = apply_scene_settings_(scene);
    // BEFORE the physics system in numeric order (50 vs 100), which is the whole point: a
    // component driving a kinematic body's Transform has to write it before PhysicsSystem reads
    // it, or physics spends the frame solving against the previous pose while the renderer draws
    // the new one. See kinematic_control_system.h's file doc.
    scene::install_kinematic_control_system(scene);
    // Order 60, so it too sits ahead of Physics (100) -- and, more to the point, ahead of the
    // Behaviour walk (200) and TransformResolve (350), so a terrain chunk that appears this
    // frame already has its world matrix resolved when the render gather reads it. A scene
    // with no Terrain component pays one empty get_components<>() sweep per frame.
    // Order 59: a map that lands this frame is applied before the streamer decides what to
    // mesh, so the first chunks are never built from an empty grid. See toyengine/mapgen.
    mapgen::install_map_generator_system(scene);
    world::install_terrain_system(scene, ctx_.device(), ctx_.allocator(), assets_);
    // Order 61: heightmap terrains -- their chunks, GPU grids and colliders, before Physics (100)
    // steps against them. See heightmap_terrain_system.h.
    world::install_heightmap_terrain_system(scene, ctx_.device(), ctx_.allocator(), ctx_.command_pool(), assets_);
    // Order 62: painted instances -- builds their chunks against ground the two terrain systems
    // above have just finished, and streams their colliders before Physics (100) reconciles.
    instance::install_instance_painter_system(scene, &assets_);
    // Order 70: destruction -- fractures breakable meshes on the job engine and spawns a
    // chunk body per cell. Before Physics (100), so a chunk born this frame falls this frame.
    scene::install_destruction_system(scene, ctx_.device(), ctx_.allocator(), assets_, &jobs_);
    // Order 90: bakes water surfaces and refreshes the buoyant-body list right before
    // Physics (100) steps, whose substep callback it drives -- see water_system.h's file doc.
    water::install_water_system(scene, &ctx_.device(), &ctx_.allocator(), &assets_)
        ->set_settings(water_settings_for_(pipeline_->render_config().water_quality));
    coopa::physx::system::install_physics_system(scene, scene_cfg.physics);
    // Orders 96 and 150: binds each Vehicle to its chassis body and drives the wheels inside
    // Physics' substep (96), then places the wheel visuals and fires its signals once the step
    // is done (150) -- see physxcoopa/system/vehicle_system.h.
    coopa::physx::system::install_vehicle_system(scene);
    // Order 95: the weather's wind (40) on cloth and wind-influenced rigidbodies, right before
    // Physics (100) steps -- see weather/wind_system.h.
    weather::install_wind_system(scene);
    // Order 150: reads the physics world (100) for its colliders, and moves NavAgents before
    // the Behaviour walk (200) -- see physxcoopa/system/nav_system.h.
    if (scene_cfg.navigation.enabled) coopa::physx::system::install_nav_system(scene, scene_cfg.navigation);
    // Order 360: after TransformResolve (350), so every emitter's world matrix is current;
    // runs in edit mode too, so effects preview live in the editor -- see
    // particle_system_runner.h.
    // `simulation: gpu` systems run in compute when particles.gpu_enabled and the device has
    // it; their alive counts come back from the renderer's readback.
    particles::install_particle_system(scene)->set_gpu(
        config_.particles.gpu_enabled && ctx_.device().supports_compute(),
        [this](uint64_t id, uint32_t& count, uint32_t& generation) {
            return pipeline_ && pipeline_->gpu_particle_alive(id, count, generation);
        });
    // Order 40: the clock, the active weather condition, sky / fog / sun and the runtime
    // weather effects -- ahead of the Behaviour walk, so gameplay reads this frame's weather.
    // Runs in edit mode too (clock held), so the editor previews it.
    {
        auto it = scene_settings_.find(&scene);
        weather::install_weather_system(scene, weather_node_(it != scene_settings_.end() ? it->second : fkyaml::node()));
    }
    // Order 45: folds the weather (40) and every EnvInfluence into the one EnvField snapshot
    // that water (90), wind (95), physics (100), gameplay (200), particles (360), snow (370)
    // and the renderer's surface-world upload all read -- see env/env_system.h for why the
    // gather is here rather than after TransformResolve.
    env::install_env_system(scene);
    // Order 44: fire. Per-object burning only -- it neither reads nor writes the environment
    // field, and there is no world fire state of any kind (see toyengine/fire/README.md).
    {
        fire::FireSystem* fs = fire::install_fire_system(scene);
        // Smoke and flame on a burn line are emitted from the burning object's own triangles,
        // which needs its mesh surfaces loaded. ParticleSystem only gets a loader from the YAML
        // parser, and these emitters are built at runtime, so the system is handed one here.
        if (fs) {
            coopa::asset::AssetManager& assets = assets_;
            fs->set_surface_loader([&assets](const std::string& key) {
                return particles::load_mesh_surface(assets, key, std::string());
            });
        }
    }

    // Order 370: after TransformResolve, so deformers stamp where they are drawn; reads the
    // weather (40) for the cover and the precipitation map -- see world/snow_system.h.
    world::install_snow_system(scene);
    // Order 365: after TransformResolve (350), so the segment it probes is the camera-to-subject
    // line actually being rendered this frame rather than last frame's -- see
    // scene/view_blocker_system.h. A scene with no ViewBlockerFade pays one component sweep.
    scene::install_view_blocker_system(scene);

    // Activate TransformSystem before the first drain or render, so the world_matrix()
    // reads below are never asked to resolve a still-dirty transform; then block until
    // every load issued above has finished, so frame 0 sees a fully populated scene.
    coopa::scene::install_transform_system(scene);
    // Every transform of a freshly built scene starts dirty, and that system's first pass is a
    // whole update away -- while the scene can be read, and drawn, before then: one pushed or
    // loaded mid-frame (the editor's Play, a game's load_scene() from a UI callback) is drawn in
    // that same frame. Resolve the tree now, so a prepared scene's world matrices are readable
    // (Transform::world_matrix()) from the moment it exists.
    coopa::scene::resolve_transforms(scene);
    // Runs at UpdatePhase::Animation (300), before TransformResolve (350) -- Scene::update()
    // orders installed systems by phase regardless of install call order, so this only needs
    // to exist before the scene starts ticking, same as install_transform_system() above.
    coopa::anim::install_animation_system(scene);
    // Order 320: TwoBoneIK / LookAtIK / FootIK over the animated pose, before TransformResolve
    // (350) -- see coopa/animation/ik_system.h. A scene with no IK pays one component sweep.
    coopa::anim::install_ik_system(scene);
    // Orders 90 / 290 / 330: Ragdoll mode changes before Physics, and the recovery blend
    // around the Animator and IK -- see toyengine/scene/ragdoll.h.
    scene::install_ragdoll_system(scene);
    // An async load skips the drain: its build already waited for the scene's own assets,
    // and whatever a system starts loading here streams in over the next frames.
    if (drain_assets) drain_pending_assets_();

    // Fail fast on a typo'd/unregistered PBRMaterial::shader -- see
    // ToyRenderPipeline::validate_material_shaders()'s doc for why this can't happen
    // during YAML parsing itself.
    pipeline_->validate_material_shaders(scene);

    scene.set_simulating(!edit_mode_);
    apply_cursor_capture_();
}

Engine::~Engine() {
    ctx_.wait_idle();
    // Before anything else: the panel seam is a non-owning process-wide pointer at assets_,
    // and the test suite builds many Engines in one process, so a stale one would outlive its
    // manager. Only clear it if it is still ours.
    if (ui::panel_assets() == &assets_) ui::set_panel_assets(nullptr);
    shutdown_audio_();
    if (save::SaveSystem::active() == &saves_) save::SaveSystem::set_active(nullptr);
    // Scenes before assets_.shutdown() below: every MeshRenderer (and texture, font, clip...)
    // holds an AssetHandle whose destructor releases its slot in the AssetManager. Left to
    // member destruction, the scenes went AFTER shutdown() had already freed those slots --
    // a use-after-free on every Engine teardown (AddressSanitizer: ~MeshRenderer writing a
    // slot freed by AssetManager::shutdown()), which corrupted the heap for whatever a
    // process did next (the editor tests' "random" segfaults).
    clear_scenes_();
    // An unfinished async load holds a built scene (asset handles) and the transition layer
    // a loading-screen UI (UIResourceCache fonts): both go before the caches below.
    cancel_scene_load_("engine shutting down");
    finish_transition_();
    transition_.release();
    // The debug overlay's scene draws with a UIResourceCache font, cleared below.
    remove_overlay_layer(debug_overlay_.has_scene() ? &debug_overlay_.scene() : nullptr);
    debug_overlay_.release_scene();
    debug::GameLines::instance().set_accepting(false);
    // register_render_components()'s parser lambdas capture ctx_'s device/allocator/
    // cmd_pool by reference in SceneLoader's function-local static registry, which
    // would otherwise only be destroyed at program exit -- after ctx_ goes out of
    // scope. Clear it now, while they're still alive. Then shut down the asset
    // manager (which holds every loaded Mesh/Texture's GPU allocation) before ctx_
    // (and the device/allocator it owns) destructs.
    coopa::scene::SceneLoader::clear_component_parsers();

    // UIResourceCache holds GPU-resident Fonts/Textures in function-local static storage,
    // so without this they would only be released at program exit -- after ctx_'s Device
    // and Allocator are gone, which trips VMA's "allocations not freed" assertion. Same
    // contract as clear_component_parsers() above; see UIResourceCache::clear()'s doc.
    coopa::ui::UIResourceCache::instance().clear();
    // font_for_path() publishes the first font it loads as the static FontDefaults::font,
    // and clear() just destroyed it without resetting that. Left dangling, the NEXT Engine
    // in this process (every editor test) hands it to its first unthemed Text as the
    // fallback font -- a use-after-free AddressSanitizer caught in mark_text_atlases().
    coopa::ui::FontDefaults::font = nullptr;
    // Same contract as UIResourceCache above: IconLibrary is a process-wide static holding
    // GPU-resident sheets through its AssetHandles, so it has to let go before assets_ and
    // ctx_ do.
    coopa::ui::IconLibrary::instance().clear();
    assets_.shutdown();
}

void Engine::set_display_region(std::optional<render::LetterboxRect> region) {
    pipeline_->set_display_region(region);
}

render::ToyRenderConfig& Engine::render_config() { return pipeline_->render_config_mut(); }

void Engine::run() {
    constexpr uint64_t kWarmupFrames = 30;
    uint32_t captured = 0;
    std::deque<coopa::gfx::util::ImageData> ring;
    uint64_t frames_run = 0;
    std::chrono::steady_clock::time_point timed_start{};
    while (!ctx_.should_close()) {
        if (!tick()) break;
        if (++frames_run == kWarmupFrames) timed_start = std::chrono::steady_clock::now();

        if (capture_frames_ > 0) {
            capture_sequence_frame_(captured);
            ++captured;
            if (captured >= capture_frames_) break;
        }
        if (capture_ring_ > 0) {
            ctx_.wait_idle();
            ring.push_back(capture_image(config_.output.save_low_res));
            if (ring.size() > capture_ring_) ring.pop_front();
        }

        if (ctx_.max_frames() > 0 && ctx_.frame_index() >= ctx_.max_frames()) break;
    }

    if (frames_run > kWarmupFrames) {
        ctx_.wait_idle();   // count the GPU work of the frames already submitted
        const double secs = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - timed_start).count();
        const double ms = secs * 1000.0 / static_cast<double>(frames_run - kWarmupFrames);
        std::printf("[toyengine] Frame time: %.2f ms (%.1f fps), mean of %llu frames after %llu warm-up\n",
                    ms, 1000.0 / ms, static_cast<unsigned long long>(frames_run - kWarmupFrames),
                    static_cast<unsigned long long>(kWarmupFrames));
        const std::string mesh_stats = pipeline_->mesh_draw_stats_summary();
        if (!mesh_stats.empty()) std::printf("[toyengine] Mesh draws/frame: %s\n", mesh_stats.c_str());
    }
    if (profile_) {
        ctx_.wait_idle();
        profile_->print_summary();
    }

    if (!ring.empty()) {
        std::filesystem::create_directories("output/seq");
        uint32_t index = 0;
        for (const auto& frame : ring) {
            char path[64];
            std::snprintf(path, sizeof(path), "output/seq/frame_%04u.png", index++);
            coopa::gfx::util::save_image_png(frame, path);
        }
        std::cout << "[toyengine] Wrote " << ring.size() << " ring-captured frames to output/seq/\n";
    }

    // Never in a shipping build: a player's game must not write screenshots on quit.
    if (!k_shipping && config_.output.save_on_exit) {
        ctx_.wait_idle();
        const std::string out = output_path_(config_.output.filepath);
        std::error_code dir_ec;
        if (const auto dir = std::filesystem::path(out).parent_path(); !dir.empty()) {
            std::filesystem::create_directories(dir, dir_ec);
        }
        save_screenshot(out, config_.output.save_low_res);
    }
}

void Engine::set_cursor_override(const glm::vec2& window_pixels) {
    cursor_pos_          = window_pixels;
    cursor_pos_override_ = true;
}

coopa::gfx::util::ImageData Engine::capture_image(bool low_res) {
    ctx_.wait_idle();
    coopa::gfx::memory::Image& src = low_res ? pipeline_->low_res_color_image()
                                             : pipeline_->final_color_image();
    return coopa::gfx::util::read_image(ctx_.device(), ctx_.allocator(), ctx_.command_pool(), src);
}

void Engine::capture_sequence_frame_(uint32_t index) {
    ctx_.wait_idle();
    // Relative, like config_.output.filepath -- resolves against the process CWD, not
    // ROOT_DIR (see save_screenshot()'s own doc / AppConfig::output.filepath's default).
    std::filesystem::create_directories("output/seq");
    char path[64];
    std::snprintf(path, sizeof(path), "output/seq/frame_%04u.png", index);
    save_screenshot(path, config_.output.save_low_res);
}

namespace {

/** @brief The cpu.scene.* profile column for a registered scene system, by its system_name(). */
render::CpuScope scene_system_scope_(const char* name) {
    using render::CpuScope;
    static const std::unordered_map<std::string_view, CpuScope> scopes = {
        {"Behaviour", CpuScope::SceneBehaviour},
        {"KinematicControl", CpuScope::SceneKinematic},
        {"Terrain", CpuScope::SceneTerrain},
        {"HeightmapTerrain", CpuScope::SceneHeightmapTerrain},
        {"Snow", CpuScope::SceneSnow},
        {"Physics", CpuScope::ScenePhysics},
        {"Animation", CpuScope::SceneAnimation},
        {"IK", CpuScope::SceneIk},
        {"Navigation", CpuScope::SceneNavigation},
        {"Particles", CpuScope::SceneParticles},
        {"Water", CpuScope::SceneWater},
        {"Weather", CpuScope::SceneWeather},
        {"Wind", CpuScope::SceneWind},
        {"TransformResolve", CpuScope::SceneTransformResolve},
        {"LateBehaviour", CpuScope::SceneLateBehaviour},
    };
    const auto it = scopes.find(name);
    return it != scopes.end() ? it->second : CpuScope::SceneOther;
}

} // namespace

bool Engine::tick() {
    using render::CpuScope;
    using render::CpuTimer;
    // Before `prof` is taken: this may create or destroy the live profile.
    sync_live_profile_();
    render::FrameProfile* prof = active_profile_();
    if (prof) {
        prof->begin_frame(profile_frame_++);
        prof->prune();
    }

    float dt = 0.0f;
    {
        CpuTimer t(prof, CpuScope::Input);
        ctx_.poll(); // window_.new_frame() + poll_events() + frame timer update.
        for (auto& fn : input_queue_) fn(ctx_.input());
        input_queue_.clear();

        if (options_.escape_quits && input_.is_down("quit", ctx_.input())) {
            ctx_.window().set_should_close(true);
        }

        dt = frame_dt_();
        apply_cursor_pos_override_();
        update_debug_overlay_();
    }
    // Before anything emits UI: drive_ui_canvases_() seeds every canvas's DrawList with the
    // pipeline's white texture, and a rebuild later in the frame would free it under that
    // frame's UI (drawn black). Uses the display region last frame's pre_render set.
    run_pending_rebuild_();
    if (scene_mgr_.has_scene()) update_fill_extent_();
    {
        CpuTimer t(prof, CpuScope::Assets);
        // While a scene loads in the background its uploads are spread over frames.
        assets_.update(dt, scene_loading() ? scene_load_->options.finalize_budget_ms : -1.0f);
        consume_scene_link_requests_();
        // Also after a failure, while the cover fades back off the running scene.
        if (scene_load_ && (!scene_load_->finished() || scene_load_->alpha > 0.0f)) update_scene_load_(dt);
    }
    update_saves_(dt);
    if (hooks_.pre_scene_update) hooks_.pre_scene_update(dt);

    {
        CpuTimer t(prof, CpuScope::SceneUpdate);
        if (scene_mgr_.has_scene() && !edit_mode_) {
            drive_camera_controller_(scene_mgr_.get_active_scene());
            drive_kinematic_controllers_(scene_mgr_.get_active_scene());
            drive_free_movers_(scene_mgr_.get_active_scene());
            drive_fire_igniters_(scene_mgr_.get_active_scene());
            drive_turntables_(scene_mgr_.get_active_scene());
            drive_terrain_diggers_(scene_mgr_.get_active_scene());
            drive_character_controllers_(scene_mgr_.get_active_scene());
            drive_vehicles_(scene_mgr_.get_active_scene());
            drive_ragdolls_(scene_mgr_.get_active_scene());
        }
        scene_mgr_.update(dt);
        if (overlay_scene_) overlay_scene_->update(dt);
        for (const OverlayLayer& l : overlay_layers_) l.scene->update(dt);
    }
    {
        CpuTimer t(prof, CpuScope::LateUpdate);
        {
            CpuTimer ui(prof, CpuScope::SceneUi);
            if (scene_mgr_.has_scene()) {
                // Between update() and late_update(), and that window is the only correct slot:
                // world matrices are current only after UpdatePhase::TransformResolve (350) has
                // run inside update(), and a canvas needs its pointer position before
                // EventSystem::process() is dispatched from inside late_update() (400).
                drive_ui_canvases_(scene_mgr_.get_active_scene(), scene_ui_placement_);
            }
            if (overlay_scene_) drive_ui_canvases_(*overlay_scene_, std::nullopt);
            for (const OverlayLayer& l : overlay_layers_) drive_ui_canvases_(*l.scene, overlay_layer_placement_(l));
        }
        // late_update() runs LateBehaviourSystem, flushes each worker's deferred
        // SceneCommandBuffer and advances Scene::frame_index(). Must precede render() so a
        // same-frame deferred spawn or destroy is reflected in what is drawn, matching
        // Unity's Update -> LateUpdate -> render order.
        scene_mgr_.late_update(dt);
        if (overlay_scene_) overlay_scene_->late_update(dt);
        // After every other scene's late_update(): the stats and the game's watch() lines
        // for this frame are all in.
        if (debug_overlay_.full() && debug_overlay_.refresh_due()) collect_debug_stats_();
        for (const OverlayLayer& l : overlay_layers_) l.scene->late_update(dt);
    }
    if (prof != nullptr && scene_mgr_.has_scene()) {
        // The active scene's per-system split of the SceneUpdate/LateUpdate phases, as its own
        // cpu.scene.* columns (overlay scenes are not included -- their systems are the UI's).
        for (const auto& timing : scene_mgr_.get_active_scene().system_timings()) {
            prof->add_cpu(scene_system_scope_(timing.name), timing.ms);
        }
    }
    update_audio_(dt);
    if (hooks_.post_late_update) hooks_.post_late_update(dt);

    if (scene_mgr_.has_scene()) {
        {
            CpuTimer t(prof, CpuScope::DynamicMeshes);
            upload_dynamic_meshes_(scene_mgr_.get_active_scene());
            gather_debug_lines_(scene_mgr_.get_active_scene());
            gather_digger_hover_(scene_mgr_.get_active_scene());
        }
        if (hooks_.pre_render) hooks_.pre_render(dt);
        // Lens blur (depth of field, tilt shift) is a play-time look: an editor's edit mode
        // draws sharp. Every frame, so it also holds across renderer rebuilds.
        pipeline_->set_lens_blur_suppressed(edit_mode_);
        sync_water_render_state_(scene_mgr_.get_active_scene());
        sync_weather_render_state_(scene_mgr_.get_active_scene());
        sync_sky_render_state_(scene_mgr_.get_active_scene(), dt);
        sync_surface_state_(scene_mgr_.get_active_scene());
        {
            // After the host's pre_render hook, like the water sync: the batches point into
            // the systems' buffers and go to THIS pipeline.
            CpuTimer t(prof, CpuScope::DynamicMeshes);
            sync_particle_render_state_(scene_mgr_.get_active_scene());
        }
        // Last thing before the draw, because everything above may still move the scene:
        // whatever wrote a transform after update()'s TransformResolve (350) -- a late_update()
        // write, a deferred spawn flushed at the end of late_update(), a host's scene build,
        // object rebuild or posed timeline frame (the editor's Play, inspector edits, animation
        // scrubbing) -- left world matrices dirty, and render()'s gather reads the cached
        // Transform::world_matrix() from several threads at once, which cannot resolve them.
        // Resolved here, the frame draws this frame's poses.
        coopa::scene::resolve_transforms(scene_mgr_.get_active_scene());
        CpuTimer t(prof, CpuScope::Render);
        pipeline_->render(ctx_.renderer(), scene_mgr_.get_active_scene(), dt);
    }
    // toy::debug::watch()/text() lines last one frame.
    debug::GameLines::instance().clear();

    // The app has drawn its own frame now (or has nothing to draw and never will), so the
    // startup screen has served its purpose.
    dismiss_boot_splash_();

    return !ctx_.should_close();
}

std::optional<Engine::ScreenUiPlacement> Engine::overlay_layer_placement_(const OverlayLayer& layer) {
    if (!layer.in_display_rect) return std::nullopt;
    const render::LetterboxRect rect = display_rect();
    if (rect.w == 0 || rect.h == 0) return std::nullopt;
    // At the display's scale, so the HUD keeps a physical size (and crisp text) on HiDPI.
    return ScreenUiPlacement{rect, layer.input, std::max(1.0f, display_scale())};
}

void Engine::update_fill_extent_() {
    const render::ToyRenderConfig& cfg = pipeline_->render_config();
    if (cfg.resolution_mode != "fill") return;
    const coopa::gfx::Extent2D sc = ctx_.swapchain().extent_typed();
    if (sc.width == 0 || sc.height == 0) return;
    const auto& region = pipeline_->display_region();
    const float rw = region ? static_cast<float>(region->w) : static_cast<float>(sc.width);
    const float rh = region ? static_cast<float>(region->h) : static_cast<float>(sc.height);
    if (rw < 2 || rh < 2) return;
    render::ToyRenderConfig want = cfg;
    want.fill_aspect = rw / rh;
    const render::RenderExtent e = render::compute_render_extent(want, sc.width, sc.height);
    if (e.width == pipeline_->render_width() && e.height == pipeline_->render_height()) {
        fill_stable_frames_ = 0;
        return;
    }
    if (e.width != fill_pending_.width || e.height != fill_pending_.height) {
        fill_pending_ = e;
        fill_stable_frames_ = 0;
    }
    if (++fill_stable_frames_ < kFillDebounceFrames) return;
    fill_stable_frames_ = 0;
    rebuild_pipeline(want);
}

render::ToyRenderConfig Engine::source_render_config_() const {
    AppConfig d = config_;
    d.render = AppConfig::from_node(config_.source).render;
    if (source_driven_ && config_adjust_) config_adjust_(d);
    return make_render_config_(d, options_.project_root);
}

void Engine::queue_rebuild_(const render::ToyRenderConfig& from, const render::ToyRenderConfig& to, int delay, bool force) {
    if (!pending_rebuild_) rebuild_from_ = from;   // a newer request keeps the first baseline
    pending_rebuild_ = to;
    rebuild_wait_ = delay;
    rebuild_forced_ = rebuild_forced_ || force;
}

void Engine::run_pending_rebuild_() {
    if (!pending_rebuild_) return;
    if (rebuild_wait_-- > 0) return;
    render::ToyRenderConfig cfg = pipeline_->render_config();
    const float fill_aspect = cfg.fill_aspect;
    bool changed = rebuild_forced_;
#define TOY_TAKE_QUEUED(field)                                                              \
    if (rebuild_forced_ || rebuild_from_.field != pending_rebuild_->field) {            \
        changed = changed || cfg.field != pending_rebuild_->field;                       \
        cfg.field = pending_rebuild_->field;                                             \
    }
    TOY_STARTUP_FIXED_FIELDS(TOY_TAKE_QUEUED)
#undef TOY_TAKE_QUEUED
    cfg.fill_aspect = fill_aspect;   // the engine's, from the display region
    pending_rebuild_.reset();
    rebuild_forced_ = false;
    if (!changed) return;
    rebuild_pipeline(cfg);
    ++pipeline_rebuilds_;
}

void Engine::rebuild_pipeline(const render::ToyRenderConfig& cfg) {
    ctx_.wait_idle();
    const std::optional<render::LetterboxRect> region = pipeline_->display_region();
    pipeline_.reset();   // free the old targets before allocating the new ones
    pipeline_ = std::make_unique<render::ToyRenderPipeline>(ctx_.device(), ctx_.allocator(), ctx_.swapchain(),
                                                              ctx_.render_pass(), ctx_.command_pool(), cfg);
    pipeline_->set_job_engine(&jobs_);
    pipeline_->set_parallel_threshold(config_.jobs.parallel_threshold);
    if (render::FrameProfile* prof = active_profile_()) pipeline_->set_profiler(prof);
    pipeline_->set_overlay_scene(overlay_scene_);
    sync_overlay_layers_();
    pipeline_->set_display_region(region);
    if (scene_mgr_.has_scene()) pipeline_->validate_material_shaders(scene_mgr_.get_active_scene());
}

void Engine::restart_renderer() {
    const AppConfig eff = scene_mgr_.has_scene() ? scene_config(scene_mgr_.get_active_scene()) : config_;
    render::ToyRenderConfig to = make_render_config_(eff, options_.project_root);
    queue_rebuild_(derived_render_ ? *derived_render_ : to, to, 0, /*force=*/true);
    derived_render_ = to;
}

coopa::scene::SceneObject* Engine::spawn(const std::string& object_asset, const glm::vec3& position,
                                 coopa::scene::SceneObject* parent) {
    if (!scene_mgr_.has_scene()) return nullptr;
    try {
        fkyaml::node overrides = fkyaml::node::mapping();
        fkyaml::node comps = fkyaml::node::sequence();
        fkyaml::node t = fkyaml::node::mapping();
        t["type"] = fkyaml::node(std::string("Transform"));
        fkyaml::node pos = fkyaml::node::mapping();
        pos["x"] = fkyaml::node(static_cast<double>(position.x));
        pos["y"] = fkyaml::node(static_cast<double>(position.y));
        pos["z"] = fkyaml::node(static_cast<double>(position.z));
        t["position"] = pos;
        comps.as_seq().push_back(t);
        overrides["components"] = comps;
        return coopa::scene::SceneLoader::spawn(scene_mgr_.get_active_scene(), object_asset, parent, &overrides);
    } catch (const std::exception& e) {
        std::cerr << "[toyengine] spawn('" << object_asset << "') failed: " << e.what() << "\n";
        return nullptr;
    }
}

namespace {

/**
 * @brief Sets the toyengine logo (rasterized from branding.h at the usual icon sizes) as the
 *        window's taskbar icon -- or, on macOS, the Dock icon.
 */
void set_window_icon(coopa::gfx::presentation::Window& window) {
    static const int kSizes[] = {16, 32, 48, 64, 128, 256, 512};
    std::vector<std::vector<uint8_t>> pixels;
    std::vector<coopa::gfx::presentation::Window::IconImage> images;
    for (int s : kSizes) pixels.push_back(rasterize_logo(s));
    for (size_t i = 0; i < pixels.size(); ++i) images.push_back({kSizes[i], kSizes[i], pixels[i].data()});
    window.set_icon(images);
}

}  // namespace

std::unique_ptr<BootSplash> Engine::make_boot_splash_(coopa::gfx::app::Context& ctx,
                                                      const AppConfig& config,
                                                      const EngineOptions& options) {
    // TOY_SPLASH=1 shows it on a never-mapped window too: the only way an automated run can
    // drive the real present path, since a test window is never visible (never in a shipping
    // build, like every other debug_env knob).
    const char* forced = debug_env("TOY_SPLASH");
    const bool visible = config.window.visible || (forced && *forced && std::string(forced) != "0");
    const bool wanted = visible && config.window.splash && options.boot_splash;
    std::unique_ptr<BootSplash> splash;
    if (wanted) {
        BootSplashStyle style;
        style.backdrop   = config.window.splash_color;
        style.title      = config.window.splash_title;
        style.logo_image = config.window.splash_image;
        const std::filesystem::path root = options.project_root.empty() ? default_project_root()
                                                                        : options.project_root;
        std::vector<std::string> asset_roots{(root / "assets").string()};
        const std::filesystem::path& engine_assets = RuntimeLayout::current().engine_assets;
        std::error_code ec;
        if (!engine_assets.empty() &&
            !std::filesystem::equivalent(root / "assets", engine_assets, ec)) {
            asset_roots.push_back(engine_assets.string());
        }
        try {
            splash = std::make_unique<BootSplash>(
                ctx.device(), ctx.allocator(), ctx.command_pool(), ctx.render_pass(),
                ctx.color_format(), coopa::gfx::pipeline::ShaderLibrary(RuntimeLayout::current().shader_roots(root)),
                asset_roots, std::move(style));
            // The first frame goes to the surface BEFORE the window is mapped, so the compositor
            // never samples an unpainted window -- that flash of whatever was on screen before is
            // the whole reason this exists. The second present covers a window system that
            // discards what was presented to an unmapped surface.
            splash->present(ctx, "Starting renderer...", 0.1f);
        } catch (const std::exception& e) {
            std::cerr << "[toyengine] No boot splash: " << e.what() << "\n";
            splash.reset();
        }
    }
    if (options.set_app_icon) set_window_icon(ctx.window());
    ctx.show_window();
    if (splash) splash->present(ctx, "Starting renderer...", 0.1f);
    return splash;
}

void Engine::boot_progress(const std::string& status, float progress, float min_interval) {
    if (!boot_splash_ || !boot_splash_->due(min_interval)) return;
    boot_splash_->present(ctx_, status, progress);
}

void Engine::dismiss_boot_splash_() {
    if (!boot_splash_) return;
    ctx_.wait_idle();   // the splash's geometry buffers are still in flight
    boot_splash_.reset();
}

coopa::gfx::app::ContextConfig Engine::make_context_config_(const AppConfig& config) {
    coopa::gfx::app::ContextConfig cc;
    cc.title      = config.window.title;
    cc.width      = config.window.width;
    cc.height     = config.window.height;
    cc.resizable  = true;
    cc.vsync      = config.window.vsync;
    cc.visible    = config.window.visible;
    // The window waits for the boot splash's first frame; make_boot_splash_() maps it, splash or
    // no splash, so this never leaves a window unmapped.
    cc.show_when_ready = config.window.splash;
#ifdef NDEBUG
    cc.validation = false;
#else
    cc.validation = true;
#endif
    // Shipping: no validation and none of gfxcoopa's scripted-run env hooks (MAX_FRAMES,
    // ONESHOT, ...) -- a player's environment must not reconfigure the game.
    if constexpr (k_shipping) {
        cc.validation = false;
        return cc;
    }
    // TOY_VALIDATION=1/0 forces the Khronos layer on/off regardless of build type.
    if (const char* v = std::getenv("TOY_VALIDATION"); v && *v) cc.validation = std::string(v) != "0";
    return coopa::gfx::app::ContextConfig::from_env(cc);
}

void Engine::bind_default_input_() {
    using coopa::input::Key;
    input_.bind("quit", Key::Escape);
    // Cycles the debug stats overlay off -> fps -> full (see update_debug_overlay_()).
    input_.bind("debug_overlay", Key::F3);
    input_.bind("quick_save", Key::F5);   // save.quick_slot (toyengine/save/)
    input_.bind("quick_load", Key::F9);

    input_.bind_axis("fly_x", Key::D, Key::A); // strafe: +right/-left
    input_.bind_axis("fly_y", Key::W, Key::S); // forward/back
    input_.bind_axis("fly_z", Key::E, Key::Q); // world up/down
    input_.bind_vector("look", Key::Right, Key::Left, Key::Up, Key::Down);

    // Object movement gets its OWN axes rather than reusing fly_x/fly_y: those belong to the
    // camera's Fly mode, and a scene with both a fly camera and a driveable object would
    // otherwise move them together on the same keypress.
    input_.bind_axis("move_x", Key::D, Key::A); // world +X / -X
    input_.bind_axis("move_y", Key::W, Key::S); // world +Y / -Y
    // Vertical, for FreeMover only -- KinematicController is planar by design and ignores it.
    // Tab/Shift rather than the more usual Space/Shift because Space is left free for a jump
    // action, which is the binding a character controller will want first.
    input_.bind_axis("move_z", Key::Tab, Key::LeftShift); // world +Z (up) / -Z (down)

    // CharacterController: WASD as one camera-relative vector, Space to jump, LeftShift to
    // sprint. `sprint` is its own action so move_z's binding above is untouched.
    input_.bind_vector("move", Key::D, Key::A, Key::W, Key::S);
    input_.bind("jump", Key::Space);
    input_.bind("sprint", Key::LeftShift);
    // Ragdoll (input_toggle): R goes limp / recovers.
    input_.bind("ragdoll", Key::R);
    // Rider: F gets on a Mount in reach / off the one ridden (E/Q belong to fly_z). Riding,
    // LeftControl holds the horse to a walk (Shift gallops, as `sprint`).
    input_.bind("interact", Key::F);
    input_.bind("walk", Key::LeftControl);
    // Vehicle (physxcoopa's Vehicle component): W/S throttle and brake and A/D steer come from
    // the same `move` vector the characters use, read in the vehicle's own frame rather than the
    // camera's. The three below share keys with `jump`/`sprint`/`walk` deliberately -- an action
    // is an independent lookup, and a scene is either being walked or driven, never both.
    input_.bind("handbrake", Key::Space);
    input_.bind("gear_up", Key::LeftShift);
    input_.bind("gear_down", Key::LeftControl);
    input_.bind("next_vehicle", Key::V);
    // R rights a vehicle that has ended up on its roof. Shares its key with `ragdoll` on the
    // same deliberate basis as the three above: an action is an independent lookup, and a scene
    // is either being driven or being walked around in, never both.
    input_.bind("recover", Key::R);
}

void Engine::drive_camera_controller_(coopa::scene::Scene& scene) {
    auto* cc = scene.find_first_component<scene::CameraController>();
    if (!cc) return;

    if (input_blocked_()) {
        cc->mouse_delta  = glm::vec2(0.0f);
        cc->scroll_input = 0.0f;
        cc->move_input   = glm::vec3(0.0f);
        cc->look_input   = glm::vec2(0.0f);
        return;
    }

    // coopa::input::Input already suppresses the spurious first-frame jump
    // GLFW's virtual cursor reports right when CursorMode::Disabled is
    // first applied -- see Input::push_cursor_position()'s doc.
    // With `look_button` set, the mouse only turns the camera while that button is held: a game
    // that keeps its pointer visible (to click a target, to drag an item) cannot have every
    // mouse move spin the world.
    const bool look_held = cc->look_button.empty() || [&] {
        using coopa::input::MouseButton;
        const std::string& b = cc->look_button;
        if (b == "left" || b == "Left") return ctx_.input().button_down(MouseButton::Left);
        if (b == "middle" || b == "Middle") return ctx_.input().button_down(MouseButton::Middle);
        return ctx_.input().button_down(MouseButton::Right);
    }();
    cc->mouse_delta = look_held ? ctx_.input().cursor_delta() : glm::vec2(0.0f);
    cc->scroll_input = ctx_.input().scroll_delta().y;

    cc->move_input = glm::vec3(
        input_.axis("fly_x", ctx_.input()),
        input_.axis("fly_y", ctx_.input()),
        input_.axis("fly_z", ctx_.input()));
    cc->look_input = input_.vector("look", ctx_.input());
}

void Engine::drive_kinematic_controllers_(coopa::scene::Scene& scene) {
    std::vector<scene::KinematicController*> controllers =
        scene.get_components<scene::KinematicController>();
    if (controllers.empty()) return;

    const glm::vec2 move = input_blocked_()
        ? glm::vec2(0.0f)
        : glm::vec2(input_.axis("move_x", ctx_.input()), input_.axis("move_y", ctx_.input()));
    for (scene::KinematicController* kc : controllers) kc->move_input = move;
}

void Engine::drive_free_movers_(coopa::scene::Scene& scene) {
    std::vector<scene::FreeMover*> movers = scene.get_components<scene::FreeMover>();
    if (movers.empty()) return;

    const glm::vec3 move = input_blocked_()
        ? glm::vec3(0.0f)
        : glm::vec3(input_.axis("move_x", ctx_.input()),
                    input_.axis("move_y", ctx_.input()),
                    input_.axis("move_z", ctx_.input()));
    for (scene::FreeMover* fm : movers) fm->move_input = move;
}

void Engine::drive_fire_igniters_(coopa::scene::Scene& scene) {
    std::vector<fire::FireIgniter*> igniters = scene.get_components<fire::FireIgniter>();
    if (igniters.empty()) return;

    using coopa::input::MouseButton;
    const bool blocked = input_blocked_();
    glm::vec3 origin(0.0f), dir(0.0f, 1.0f, 0.0f);
    const bool have_ray = viewport_ray(ctx_.input().cursor_position(), origin, dir);

    for (fire::FireIgniter* g : igniters) {
        MouseButton button = MouseButton::Left;
        if (g->button == "right") button = MouseButton::Right;
        else if (g->button == "middle") button = MouseButton::Middle;

        g->ignite_requested = !blocked && have_ray && ctx_.input().button_pressed(button);
        // keys.h has a Key -> name table but no name -> Key, so the component's `reset_key`
        // is matched against it rather than a second table being invented here.
        bool reset = false;
        if (!blocked && !g->reset_key.empty()) {
            for (int k = 0; k < static_cast<int>(coopa::input::Key::Count); ++k) {
                const auto key = static_cast<coopa::input::Key>(k);
                if (g->reset_key == coopa::input::key_name(key)) {
                    reset = ctx_.input().key_pressed(key);
                    break;
                }
            }
        }
        g->reset_requested = reset;
        g->ray_origin = origin;
        g->ray_direction = dir;
    }
}

void Engine::drive_turntables_(coopa::scene::Scene& scene) {
    std::vector<scene::Turntable*> turntables = scene.get_components<scene::Turntable>();
    if (turntables.empty()) return;

    // The camera's own right axis, so a drag "across the screen" spins the model across the
    // screen from wherever the camera happens to have been placed; world +X (what an orbit
    // camera at yaw 0 looks across) if the scene has no camera yet.
    glm::vec3 right(1.0f, 0.0f, 0.0f);
    if (auto* cam = coopa::gfx::engine::components::CameraComponent::main()) {
        const glm::mat4 world = glm::inverse(cam->get_view_matrix());
        if (glm::length(glm::vec3(world[0])) > 1e-4f) right = glm::normalize(glm::vec3(world[0]));
    }

    const bool blocked = input_blocked_();
    for (scene::Turntable* tt : turntables) {
        const bool held = !blocked && (tt->drag_button.empty() || [&] {
            using coopa::input::MouseButton;
            const std::string& b = tt->drag_button;
            if (b == "left" || b == "Left") return ctx_.input().button_down(MouseButton::Left);
            if (b == "middle" || b == "Middle") return ctx_.input().button_down(MouseButton::Middle);
            return ctx_.input().button_down(MouseButton::Right);
        }());
        tt->dragging = held;
        tt->drag_input = held ? ctx_.input().cursor_delta() : glm::vec2(0.0f);
        tt->view_right = right;
    }
}

void Engine::drive_terrain_diggers_(coopa::scene::Scene& scene) {
    std::vector<scene::TerrainDigger*> diggers = scene.get_components<scene::TerrainDigger>();
    if (diggers.empty()) return;

    using coopa::input::Key;
    using coopa::input::MouseButton;
    const bool blocked = input_blocked_();
    // Where the player is pointing: the cursor, unless the camera has taken it -- a scene that
    // hides the pointer (CameraController::capture_cursor) aims from the middle of the view,
    // the way a crosshair does, and its virtual cursor position means nothing on screen.
    const render::LetterboxRect box = display_rect();
    const auto* cc = scene.find_first_component<scene::CameraController>();
    const bool crosshair = cc != nullptr && cc->capture_cursor;
    const glm::vec2 centre(static_cast<float>(box.x) + 0.5f * static_cast<float>(box.w),
                           static_cast<float>(box.y) + 0.5f * static_cast<float>(box.h));
    const glm::vec2 aim = crosshair ? centre : ctx_.input().cursor_position();
    glm::vec3 origin(0.0f), dir(0.0f);
    bool have_ray = !blocked && viewport_ray(aim, origin, dir);
    // A cursor that does not land inside the viewport at all -- a window whose scaling the
    // cursor units do not match, a pointer that has not moved since the window opened -- would
    // otherwise leave the tool silently dead. Aim down the middle instead: the hover outline
    // then shows which tile is targeted, which is the whole point of having it.
    if (!blocked && !have_ray) have_ray = viewport_ray(centre, origin, dir);

    for (scene::TerrainDigger* d : diggers) {
        const bool held = have_ray && [&] {
            const std::string& b = d->dig_button;
            if (b == "left" || b == "Left") return ctx_.input().button_down(MouseButton::Left);
            if (b == "right" || b == "Right") return ctx_.input().button_down(MouseButton::Right);
            return ctx_.input().button_down(MouseButton::Middle);
        }();
        const bool raise_held = !blocked && !d->raise_key.empty() && [&] {
            const std::string& k = d->raise_key;
            if (k == "ctrl" || k == "control") return ctx_.input().key_down(Key::LeftControl) || ctx_.input().key_down(Key::RightControl);
            if (k == "alt") return ctx_.input().key_down(Key::LeftAlt) || ctx_.input().key_down(Key::RightAlt);
            return ctx_.input().key_down(Key::LeftShift) || ctx_.input().key_down(Key::RightShift);
        }();
        const bool raise = held && raise_held;
        d->digging = held;
        d->raising = raise || (have_ray && !d->raise_key.empty() && raise_held);
        d->ray_origin = origin;
        d->ray_direction = have_ray ? dir : glm::vec3(0.0f);
    }
}

void Engine::drive_character_controllers_(coopa::scene::Scene& scene) {
    std::vector<scene::CharacterController*> characters = scene.get_components<scene::CharacterController>();
    std::vector<scene::Mount*> mounts = scene.get_components<scene::Mount>();
    std::vector<scene::Rider*> riders = scene.get_components<scene::Rider>();
    if (characters.empty() && mounts.empty() && riders.empty()) return;

    float yaw = 0.0f;
    glm::vec3 basis_forward(0.0f);   // the camera's view, for characters in a gravity field
    if (auto* cam = coopa::gfx::engine::components::CameraComponent::main()) {
        const glm::mat4 world = glm::inverse(cam->get_view_matrix());
        glm::vec3 fwd = -glm::vec3(world[2]);            // cameras look down local -Z
        // Looking (nearly) straight down a field's up, the screen's up is the way forward; the
        // character lays this onto its own ground plane, which takes the view along otherwise.
        basis_forward = fwd + glm::vec3(world[1]) * 0.25f;
        if (std::abs(fwd.z) > 0.99f) fwd = glm::vec3(world[1]); // looking straight down: screen-up
        if (glm::length(glm::vec2(fwd.x, fwd.y)) > 1e-4f) yaw = glm::degrees(std::atan2(-fwd.x, fwd.y));
    }
    const bool blocked = input_blocked_();
    const glm::vec2 move = blocked ? glm::vec2(0.0f) : input_.vector("move", ctx_.input());
    const bool jump = !blocked && input_.is_pressed("jump", ctx_.input());
    const bool sprint = !blocked && input_.is_down("sprint", ctx_.input());
    const bool walk = !blocked && input_.is_down("walk", ctx_.input());
    const bool interact = !blocked && input_.is_pressed("interact", ctx_.input());
    // Only the player's own body: not a horse (player_input off -- its Mount takes the input
    // below while ridden), nor a character handed over to a ragdoll or a saddle.
    for (scene::CharacterController* cc : characters) {
        if (!cc->player_input || cc->is_suspended()) continue;
        cc->move_input = move;
        cc->move_basis_yaw_deg = yaw;
        cc->move_basis_forward = basis_forward;
        cc->jump = cc->jump || jump;
        cc->sprint = sprint;
    }
    for (scene::Mount* m : mounts) {
        const bool ridden = m->is_ridden();
        m->move_input = ridden ? move : glm::vec2(0.0f);
        m->move_basis_yaw_deg = yaw;
        m->sprint = ridden && sprint;
        m->walk = ridden && walk;
        m->jump = m->jump || (ridden && jump);
    }
    for (scene::Rider* r : riders) r->interact = r->interact || interact;
}

void Engine::drive_vehicles_(coopa::scene::Scene& scene) {
    using coopa::physx::components::VehicleComponent;
    std::vector<VehicleComponent*> vehicles = scene.get_components<VehicleComponent>();
    if (vehicles.empty()) return;

    // The ones the player may drive, in scene order -- what V cycles through.
    std::vector<VehicleComponent*> driveable;
    for (VehicleComponent* v : vehicles) {
        if (v->player_input) driveable.push_back(v);
    }
    if (driveable.empty()) return;

    const bool blocked = input_blocked_();
    if (!blocked && input_.is_pressed("next_vehicle", ctx_.input())) {
        std::size_t current = 0;
        for (std::size_t i = 0; i < driveable.size(); ++i) {
            if (driveable[i]->active) current = i;
        }
        const std::size_t next = (current + 1) % driveable.size();
        for (VehicleComponent* v : driveable) v->active = false;
        driveable[next]->active = true;
        // Follow the new vehicle without disturbing where the camera is looking from: `tracker`
        // is resolved by name every frame, so moving it keeps yaw, pitch and distance.
        if (auto* cam = scene.find_first_component<scene::CameraController>()) {
            if (!cam->tracker.empty() && driveable[next]->owner) {
                cam->tracker = driveable[next]->owner->name();
            }
        }
    }

    const glm::vec2 move = blocked ? glm::vec2(0.0f) : input_.vector("move", ctx_.input());
    const float handbrake = (!blocked && input_.is_down("handbrake", ctx_.input())) ? 1.0f : 0.0f;
    int gear_request = 0;
    if (!blocked && input_.is_pressed("gear_up", ctx_.input())) gear_request = 1;
    if (!blocked && input_.is_pressed("gear_down", ctx_.input())) gear_request = -1;
    const bool recover = !blocked && input_.is_pressed("recover", ctx_.input());

    for (VehicleComponent* v : driveable) {
        if (!v->active) continue;  // VehicleSystem zeroes the controls of the rest
        v->controls.throttle = std::max(move.y, 0.0f);
        v->controls.brake = std::max(-move.y, 0.0f);
        v->controls.steer = move.x;
        v->controls.handbrake = handbrake;
        // OR-ed in, not overwritten: VehicleSystem clears these after the step that used them,
        // so a press is never lost to a frame whose physics did not run.
        if (gear_request != 0) v->controls.gear_request = gear_request;
        v->controls.recover = v->controls.recover || recover;
    }
}

void Engine::drive_ragdolls_(coopa::scene::Scene& scene) {
    if (input_blocked_() || !input_.is_pressed("ragdoll", ctx_.input())) return;
    for (scene::Ragdoll* r : scene.get_components<scene::Ragdoll>()) {
        // Not in the saddle: the Rider owns the body until it is back on foot.
        auto* rider = r->owner ? r->owner->get_component<scene::Rider>() : nullptr;
        if (rider && rider->is_mounted()) continue;
        if (r->input_toggle) r->toggle();
    }
}

void Engine::upload_dynamic_meshes_(coopa::scene::Scene& scene) {
    for (scene::ClothRenderer* cr : scene.get_components<scene::ClothRenderer>()) {
        cr->upload(ctx_.current_frame());
    }
    // Same per-frame-in-flight contract as ClothRenderer above. With GPU skinning each
    // upload() only builds its palette and queues a dispatch that the pipeline records at
    // the top of this frame's command buffer; otherwise it CPU-skins and uploads.
    render::passes::SkinningPass* gpu_skin = pipeline_ ? pipeline_->skinning_pass() : nullptr;
    if (gpu_skin) gpu_skin->begin_frame();
    for (scene::SkinnedMeshRenderer* smr : scene.get_components<scene::SkinnedMeshRenderer>()) {
        smr->upload(ctx_.current_frame(), gpu_skin);
    }
}

void Engine::gather_digger_hover_(coopa::scene::Scene& scene) {
    for (scene::TerrainDigger* d : scene.get_components<scene::TerrainDigger>()) {
        if (!d->show_hover || !d->hovering()) continue;
        const glm::vec3& c = d->raising ? d->raise_color : d->hover_color;
        const auto byte = [](float v) {
            return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        // pack_gpu_color() takes physxcoopa's 0xRRGGBBAA packing, which is what this builds.
        const uint32_t packed = render::pack_gpu_color((byte(c.r) << 24) | (byte(c.g) << 16) |
                                                       (byte(c.b) << 8) | 0xFFu);
        const auto& p = d->hover_corners();
        const glm::vec3 centre = 0.25f * (p[0] + p[1] + p[2] + p[3]);
        const glm::vec3 up = glm::normalize(glm::cross(p[1] - p[0], p[3] - p[0]));
        // X-ray (occluded = false): the outline hugs the surface it marks, and depth-tested it
        // would z-fight its way out of existence.
        const auto line = [&](const glm::vec3& a, const glm::vec3& b) {
            pipeline_->debug_lines().push_back(render::DebugLine{a, b, packed, false});
        };
        // Three rings, each a little further in: one line is a single pixel at the resolutions
        // this engine renders at, and a single pixel over a busy terrace is not a cue anybody
        // sees. Drawn toward the centre rather than outward so the band stays on the tiles the
        // click will actually take.
        for (int ring = 0; ring < 3; ++ring) {
            const float t = 0.06f * static_cast<float>(ring);
            glm::vec3 q[4];
            for (int i = 0; i < 4; ++i) q[i] = glm::mix(p[static_cast<std::size_t>(i)], centre, t);
            for (int i = 0; i < 4; ++i) line(q[i], q[(i + 1) % 4]);
        }
        // Corner posts: the outline read from a low angle, where the ground is nearly edge-on.
        const float post = 0.45f * glm::length(p[1] - p[0]);
        for (int i = 0; i < 4; ++i) line(p[static_cast<std::size_t>(i)], p[static_cast<std::size_t>(i)] + up * post);
    }
}

bool Engine::ui_wants_pointer() {
    // Asked before a click is allowed to reach the world: a panel's background exists to stop
    // one, and so does an open popup. An immediate pass answers from the layout it drew last
    // frame -- the same one the widgets acted on, so the click cannot be claimed by one and
    // used by the other.
    if (!scene_mgr_.has_scene()) return false;
    for (coopa::ui::ImmediateCanvas* ui :
         scene_mgr_.get_active_scene().get_components<coopa::ui::ImmediateCanvas>()) {
        if (ui->wants_pointer()) return true;
    }
    return false;
}

void Engine::drive_ui_canvases_(coopa::scene::Scene& scene, const std::optional<ScreenUiPlacement>& placement) {
    std::vector<coopa::ui::CanvasComponent*> canvases = coopa::ui::collect_canvases(scene);
    if (canvases.empty()) return;
    // A scene that is drawn but not simulating (the editor's edit mode) never runs a
    // canvas's late_update(), which is where layout and emit happen. Whether it is previewed
    // anyway is the host's call (set_ui_preview()): an editor that designs interfaces in their
    // own workspace keeps them out of the scene view, and clears the DrawList rather than
    // leaving a frozen copy of the last frame in it.
    const bool preview = !scene.is_simulating();

    const uint32_t rw = pipeline_->render_width();
    const uint32_t rh = pipeline_->render_height();
    const uint32_t ww = ctx_.swapchain().extent().width;
    const uint32_t wh = ctx_.swapchain().extent().height;
    const coopa::gfx::TextureView white        = pipeline_->world_ui_white_view();
    const coopa::gfx::TextureView screen_white = pipeline_->screen_ui_white_view();

    auto* cam = scene.find_first_component<coopa::gfx::engine::components::CameraComponent>();
    const float aspect = rh > 0 ? static_cast<float>(rw) / static_cast<float>(rh) : 1.0f;
    const glm::mat4 view = cam ? cam->get_view_matrix() : glm::mat4(1.0f);
    const glm::mat4 proj = cam ? cam->get_projection_matrix(aspect) : glm::mat4(1.0f);

    bool ray_valid = false;
    glm::vec3 ray_origin(0.0f);
    glm::vec3 ray_dir(0.0f);
    for (coopa::ui::CanvasComponent* canvas : canvases) {
        if (!canvas->is_world_space()) {
            // Same seeding as the world path below, and for the same reason: without it
            // a screen-space canvas emits every solid-colour quad against a null view.
            canvas->set_default_texture(screen_white);
            if (placement && placement->rect.w > 0 && placement->rect.h > 0) {
                // Placed inside part of the window (see set_scene_ui_placement()).
                const float zoom = placement->zoom > 0.0f ? placement->zoom : 1.0f;
                canvas->set_viewport(std::max(1u, static_cast<uint32_t>(std::lround(placement->rect.w / zoom))),
                                     std::max(1u, static_cast<uint32_t>(std::lround(placement->rect.h / zoom))));
                canvas->set_screen_origin(glm::vec2(placement->rect.x, placement->rect.y));
                canvas->set_display_zoom(zoom);
                canvas->set_input_enabled(placement->input);
            } else {
                // The WINDOW extent, not the render extent: UiPass draws this canvas into the
                // swapchain-sized overlay target, and ctx_.input()'s cursor is in window
                // pixels, so this is the sizing that makes both agree. See the doc above.
                canvas->set_viewport(ww, wh);
                canvas->set_screen_origin(glm::vec2(0.0f));
                canvas->set_display_zoom(1.0f);
                canvas->set_input_enabled(true);
            }
            // The player's interface scale (set_ui_scale()). Applied here rather than written
            // into the asset so one slider moves every screen at once, and only to canvases
            // the window owns: a PLACED canvas is a preview, and a designer sizing a HUD must
            // see the size they authored, not the size this machine's player prefers.
            canvas->set_ui_scale(canvas->ignore_ui_scale || placement ? 1.0f : ui_scale_);
            // Cursor positions arrive in screen points; the canvas is sized in framebuffer
            // pixels. They differ on HiDPI displays.
            canvas->set_input(ctx_.input(), cursor_scale());
            if (preview) {
                if (ui_preview_) canvas->preview_refresh();
                else canvas->clear_emitted();
            }
            continue;
        }
        // Seed the DrawList's default texture BEFORE late_update() emits against it:
        // every solid-colour quad (panels, bar fills) is drawn with this view, and an
        // unseeded DrawList submits draws bound to a null image view. Idempotent and
        // cheap, and done per frame rather than once so a scene loaded later is covered.
        canvas->set_default_texture(white);
        canvas->update_world_transform(view);
        if (!ray_valid) {
            ray_valid = build_pointer_ray_(view, proj, rw, rh, ray_origin, ray_dir);
        }
        std::optional<glm::vec2> hit =
            ray_valid ? canvas->ray_to_canvas(ray_origin, ray_dir) : std::nullopt;
        // A miss must land far OUTSIDE the canvas. (0, 0) would be the canvas's own
        // bottom-left corner, which would leave whatever widget sits there permanently
        // hovered whenever the pointer is anywhere else.
        canvas->set_world_input(ctx_.input(), hit ? *hit : glm::vec2(-1.0e6f));
        if (preview) {
            if (ui_preview_) canvas->preview_refresh();
            else canvas->clear_emitted();
        }
    }
}

bool Engine::build_pointer_ray_(const glm::mat4& view, const glm::mat4& proj,
                        uint32_t rw, uint32_t rh,
                        glm::vec3& out_origin, glm::vec3& out_dir) {
    if (rw == 0 || rh == 0) return false;
    render::LetterboxRect box = display_rect();
    if (box.w == 0 || box.h == 0) return false;

    glm::vec2 cursor = ctx_.input().cursor_position() * cursor_scale() - glm::vec2(box.x, box.y);
    // Divide by box.w/h rather than box.scale: under "fit" mode w/h are rounded to whole
    // pixels, so w/rw and scale differ slightly, and w/h is what the blit actually used.
    glm::vec2 px(cursor.x * static_cast<float>(rw) / static_cast<float>(box.w),
                 cursor.y * static_cast<float>(rh) / static_cast<float>(box.h));
    glm::vec2 ndc(2.0f * px.x / static_cast<float>(rw) - 1.0f,
                  1.0f - 2.0f * px.y / static_cast<float>(rh));

    glm::mat4 inv_vp = glm::inverse(proj * view);
    glm::vec4 near_h = inv_vp * glm::vec4(ndc, 0.0f, 1.0f);
    glm::vec4 far_h  = inv_vp * glm::vec4(ndc, 1.0f, 1.0f);
    if (std::abs(near_h.w) < 1e-9f || std::abs(far_h.w) < 1e-9f) return false;
    out_origin = glm::vec3(near_h) / near_h.w;
    // Left unnormalized on purpose: ray_to_canvas() only ever uses ratios of dot
    // products, so scale cancels -- and for an orthographic camera the near->far
    // difference IS the (constant) direction.
    out_dir = glm::vec3(far_h) / far_h.w - out_origin;
    return true;
}

void Engine::apply_cursor_capture_() {
    if (no_input_ || !config_.window.visible || edit_mode_) return;
    if (!scene_mgr_.has_scene()) return;
    auto* cc = scene_mgr_.get_active_scene().find_first_component<scene::CameraController>();
    if (cc && cc->capture_cursor) {
        ctx_.input().set_cursor_mode(coopa::input::CursorMode::Disabled);
    }
}

std::string Engine::resolve_scene_path_(const std::string& path) const {
    const std::string resolved = resolve_path_(path);
    if (path.empty() || coopa::yaml::document_exists(resolved)) return resolved;
    std::string rel = std::filesystem::path(path).generic_string();
    if (rel.rfind("assets/", 0) == 0) rel = rel.substr(7);
    if (auto found = coopa::asset::AssetIndex::find_in(asset_roots(), rel)) return found->string();
    return resolved;
}

std::string Engine::resolve_against_(const std::filesystem::path& root, const std::string& path) {
    if (path.empty() || std::filesystem::path(path).is_absolute()) return path;
    return (root / path).string();
}

std::string Engine::output_path_(const std::string& path) {
    if (path.empty() || std::filesystem::path(path).is_absolute() || !RuntimeLayout::current().packaged()) {
        return path;
    }
    return (user_data_dir() / path).string();
}

std::string Engine::resolve_asset_file_(const std::filesystem::path& root, const std::string& path) {
    const std::string in_project = resolve_against_(root, path);
    if (in_project.empty() || std::filesystem::path(path).is_absolute()) return in_project;
    std::error_code ec;
    if (std::filesystem::exists(in_project, ec)) return in_project;
    // engine_assets is <checkout>/assets; `path` is project-relative (assets/palettes/...).
    const std::filesystem::path& engine_assets = RuntimeLayout::current().engine_assets;
    if (engine_assets.empty()) return in_project;   // packaged: nothing outside the package
    const std::filesystem::path in_engine = engine_assets.parent_path() / path;
    return std::filesystem::exists(in_engine, ec) ? in_engine.string() : in_project;
}

void Engine::drain_pending_assets_() {
    size_t peak = assets_.pending_load_count();
    while (assets_.pending_load_count() > 0) {
        assets_.update(0.0f);
        // The one startup stage whose progress is actually measurable. Throttled, and only alive
        // while the splash is: a present per iteration would pace the drain to the refresh rate.
        peak = std::max(peak, assets_.pending_load_count());
        boot_progress("Loading assets...",
                      0.4f + 0.55f * (1.0f - static_cast<float>(assets_.pending_load_count()) /
                                                 static_cast<float>(std::max<size_t>(peak, 1))),
                      1.0f / 30.0f);
        std::this_thread::yield();
    }

    if (!scene_mgr_.has_scene()) return;
    for (auto* mr : scene_mgr_.get_active_scene()
                         .get_components<coopa::gfx::engine::components::MeshRenderer>()) {
        const auto& handle = mr->get_mesh();
        if (handle.is_failed()) {
            std::cerr << "[toyengine] Failed to load mesh '" << mr->mesh_path()
                      << "': " << handle.error() << std::endl;
        }
    }
}

float Engine::fixed_dt_from_env_() {
    if (const char* v = debug_env("FIXED_DT")) return std::strtof(v, nullptr);
    return -1.0f;
}

uint32_t Engine::capture_frames_from_env_() {
    if (const char* v = debug_env("CAPTURE_FRAMES")) return static_cast<uint32_t>(std::atoll(v));
    return 0;
}

uint32_t Engine::capture_ring_from_env_() {
    if (const char* v = debug_env("CAPTURE_RING")) return static_cast<uint32_t>(std::atoll(v));
    return 0;
}

void Engine::apply_cursor_pos_override_() {
    if (!cursor_pos_override_) return;
    ctx_.input().push_cursor_position(cursor_pos_.x, cursor_pos_.y);
}

void Engine::read_cursor_pos_override_() {
    const char* v = debug_env("CURSOR_POS");
    if (!v || !*v) return;
    float x = 0.0f, y = 0.0f;
    if (std::sscanf(v, "%f,%f", &x, &y) == 2) {
        cursor_pos_ = glm::vec2(x, y);
        cursor_pos_override_ = true;
    }
}

std::string Engine::scene_path_from_env_(const std::string& configured) {
    const char* v = debug_env("SCENE");
    if (!v || !*v) return configured;
    std::string scene(v);
    if (coopa::yaml::is_document_ext(scene)) return scene;
    return "assets/scenes/" + scene + "/scene.yaml";
}

bool Engine::no_input_from_env_() {
    const char* v = debug_env("NO_INPUT");
    return v && *v && std::string(v) != "0";
}

render::ToyRenderConfig Engine::make_render_config_(const AppConfig& config,
                                                     const std::filesystem::path& project_root) {
    render::ToyRenderConfig rc = config.render;
    // From source: a project's own assets/shaders in front of everything (first-match-wins,
    // so a project shader replaces the engine's of the same name -- see
    // cmake/ToyProject.cmake), then the engine's, gfxcoopa's (SMAA + shared gfx/ headers)
    // and uicoopa's UI shaders -- the runtime mirror of the glslc -I search order
    // (assets/shaders/.glslc_flags). Those three library directories hold no same-named
    // files, so their relative order only matters for a project override. Packaged: the
    // one directory the packager merged those layers into, in that same precedence. See
    // RuntimeLayout.
    const std::vector<std::string> shader_roots = RuntimeLayout::current().shader_roots(project_root);
    rc.shader_dir = RuntimeLayout::current().packaged()
        ? shader_roots.front()
        : (RuntimeLayout::current().engine_assets / "shaders").string();
    rc.shaders = coopa::gfx::pipeline::ShaderLibrary(shader_roots);
    if (!rc.palette_path.empty()) rc.palette_path = resolve_asset_file_(project_root, rc.palette_path);
    if (!rc.grading_lut_path.empty()) rc.grading_lut_path = resolve_asset_file_(project_root, rc.grading_lut_path);

    // Derived surface shaders this app ships -- see gfx/surface/*.glsl. A scene material
    // opts in via `shader: <name>` (see PBRMaterial::shader); a material that never sets it
    // is completely unaffected by this registry existing.
    rc.surface_shaders.add({
        /* name  */ "foliage",
        /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,
        /* vert  */ "foliage.vert",
        /* frag  */ "foliage.frag",  // stock backbone, but back faces keep the (upward) normal
        /* shadow_vert */ "foliage_shadow.vert",
        /* shadow_frag */ "", // reuses stock shadow_depth.frag
        /* shadow_cube_vert */ "foliage_shadow_cube.vert",
        /* shadow_cube_frag */ "", // reuses stock shadow_cube.frag
        /* cull */ coopa::gfx::CullMode::None, // two-sided card, not a closed opaque solid
        /* tesc */ "",
        /* tese */ "foliage.tese",
        /* shadow_tese */ "foliage_shadow.tese",
        /* shadow_cube_tese */ "foliage_shadow_cube.tese",
    });
    rc.surface_shaders.add({
        /* name  */ "terrain_styled",
        /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,
        /* vert  */ "",  // stock gbuffer.vert
        /* frag  */ "terrain_styled.frag", // kind colour from the atlas, procedural world-space detail
        /* shadow_vert */ "",
        /* shadow_frag */ "",
        /* shadow_cube_vert */ "",
        /* shadow_cube_frag */ "",
        /* cull */ coopa::gfx::CullMode::Back,
    });
    // A planet TileTerrain's chunks: terrain_styled built with TERRAIN_PLANET (terrain_styled.glsl).
    rc.surface_shaders.add({
        /* name  */ "terrain_styled_planet",
        /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,
        /* vert  */ "",
        /* frag  */ "terrain_styled_planet.frag",
        /* shadow_vert */ "",
        /* shadow_frag */ "",
        /* shadow_cube_vert */ "",
        /* shadow_cube_frag */ "",
        /* cull */ coopa::gfx::CullMode::Back,
    });
    rc.surface_shaders.add({
        /* name  */ "triplanar",
        /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,
        /* vert  */ "triplanar.vert", // object-space position/normal for local mode
        /* frag  */ "triplanar.frag", // world/object-space triplanar maps -- see its doc
        /* shadow_vert */ "", // stock: no displacement, and an opaque caster reads no maps
        /* shadow_frag */ "",
        /* shadow_cube_vert */ "",
        /* shadow_cube_frag */ "",
        /* cull */ coopa::gfx::CullMode::Back,
        /* tesc */ "",
        /* tese */ "triplanar.tese",
    });
    {
        // The sculptable heightmap terrain (toyengine/world/heightmap_terrain.h): a shared flat
        // patch placed on the height grid in every stage, its own control stages so edges are
        // sized from the placed points -- see heightmap_terrain_surface.glsl.
        coopa::gfx::pipeline::SurfaceShaderDesc d;
        d.name = "heightmap_terrain";
        d.domain = coopa::gfx::pipeline::SurfaceShaderDomain::Opaque;
        d.vert = "heightmap_terrain.vert";
        d.frag = "heightmap_terrain.frag";
        d.shadow_vert = "heightmap_terrain_shadow.vert";
        d.shadow_cube_vert = "heightmap_terrain_shadow_cube.vert";
        d.cull = coopa::gfx::CullMode::Back;
        d.tesc = "heightmap_terrain.tesc";
        d.tese = "heightmap_terrain.tese";
        d.shadow_tese = "heightmap_terrain_shadow.tese";
        d.shadow_cube_tese = "heightmap_terrain_shadow_cube.tese";
        d.shadow_tesc = "heightmap_terrain_shadow.tesc";
        d.shadow_cube_tesc = "heightmap_terrain_shadow_cube.tesc";
        rc.surface_shaders.add(std::move(d));
    }
    rc.surface_shaders.add({
        /* name  */ "editor_paint",   // the toyeditor's Vertex / Weight Paint display -- see
        /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,   // editor_paint.vert
        /* vert  */ "editor_paint.vert",   // colour packed in the uv / tangent.w of an
        /* frag  */ "editor_paint.frag",   // editor-built preview mesh; nothing else uses it
        /* shadow_vert */ "",
        /* shadow_frag */ "",
        /* shadow_cube_vert */ "",
        /* shadow_cube_frag */ "",
        /* cull */ coopa::gfx::CullMode::Back,
    });
    rc.surface_shaders.add({
        /* name  */ "snow",   // deep snow: raised by the weather's cover, carved by SnowDeformers
        /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,   // -- see snow_surface.glsl
        /* vert  */ "snow.vert",
        /* frag  */ "snow.frag",
        /* shadow_vert */ "snow_shadow.vert",   // the raised snow casts the shadow it is drawn with
        /* shadow_frag */ "",
        /* shadow_cube_vert */ "snow_shadow_cube.vert",
        /* shadow_cube_frag */ "",
        /* cull */ coopa::gfx::CullMode::Back,
        /* tesc */ "",
        /* tese */ "snow.tese",
        /* shadow_tese */ "snow_shadow.tese",
        /* shadow_cube_tese */ "snow_shadow_cube.tese",
    });
    rc.surface_shaders.add({
        /* name  */ "water",
        /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Transparent,
        /* vert  */ "water.vert",
        /* frag  */ "water.frag",
        /* shadow_vert */ "", // Transparent domain -- unused (BLEND only shadows at alpha==1.0,
        /* shadow_frag */ "", //   and this demo's water is always translucent -- see water_surface.glsl)
        /* shadow_cube_vert */ "",
        /* shadow_cube_frag */ "",
        /* cull */ coopa::gfx::CullMode::None, // two-sided: a camera under the surface
                                               // sees its underside (Snell's window) --
                                               // see water_surface.glsl.
        /* tesc */ "water.tesc", // wider cull margin for the waves' reach
        /* tese */ "water.tese",
    });
    // A planet's water (WaterBody::local_frame): the same shader compiled with WATER_LOCAL_FRAME,
    // so "water" itself carries none of the planet code -- see water_surface.glsl.
    rc.surface_shaders.add({
        /* name  */ "water_planet",
        /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Transparent,
        /* vert  */ "water_planet.vert",
        /* frag  */ "water_planet.frag",
        /* shadow_vert */ "",
        /* shadow_frag */ "",
        /* shadow_cube_vert */ "",
        /* shadow_cube_frag */ "",
        /* cull */ coopa::gfx::CullMode::None,
        /* tesc */ "water.tesc",
        /* tese */ "water_planet.tese",
    });

    return rc;
}

} // namespace core
} // namespace toy
