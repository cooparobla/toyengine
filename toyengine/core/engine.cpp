#include <toyengine/core/engine.h>

#include <toyengine/audio/audio_system.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/scene/character_controller.h>
#include <toyengine/scene/kinematic_controller.h>
#include <toyengine/scene/ragdoll.h>
#include <toyengine/scene/skinned_mesh_renderer.h>

namespace toy {
namespace core {

Engine::Engine(AppConfig config, EngineOptions options)
    : options_(normalize_options_(std::move(options))),
      config_(std::move(config)),
      jobs_(config_.jobs.worker_threads ? config_.jobs.worker_threads
                                         : std::thread::hardware_concurrency()),
      ctx_(make_context_config_(config_)),
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
    if (options_.set_app_icon) apply_app_icon_();
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
    // Pixel-art texture sampling. texel_aa (the default): LINEAR + clamp-to-edge
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
    // "SnowDeformer" (toyengine/world/snow_system.h): objects pressing trails into deep snow.
    world::register_snow_components();
    // "SaveId" (a stable identity in saves) and the demo "SaveDemo" (toyengine/save/).
    save::register_save_components();
    // The GPU overload, so font/sprite paths in scene YAML load on demand and
    // FontDefaults::resolve_font is wired for themes. Must precede load_scene(), like
    // every other parser registration above. Its captured device/allocator references
    // are released by the SceneLoader::clear_component_parsers() already in ~Engine().
    coopa::ui::register_ui_components(ctx_.device(), ctx_.allocator(), ctx_.command_pool());
    // Registers the AnimationClip asset loader and the "Animator" component parser --
    // blendy exports skeletal/object animation as baked Transform keyframe tracks (there is
    // no vertex skinning in this engine), referenced from a scene's `Animator` component.
    // Must precede load_scene(), like every other parser registration above.
    coopa::anim::register_animation_components(assets_);
    init_audio_();
    init_saves_();
    // Project modules (TOY_MODULE in a project's src/, see module.h) last, so they can
    // build on -- or replace -- any parser registered above.
    for (const Module& m : modules()) {
        if (m.on_engine_init) m.on_engine_init(*this);
    }

    if (options_.load_default_scene) {
        load_scene(scene_path_from_env_(config_.scene.default_scene));
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

void Engine::add_overlay_layer(coopa::scene::Scene* scene, int order, bool in_display_rect) {
    if (!scene) return;
    std::erase_if(overlay_layers_, [scene](const OverlayLayer& l) { return l.scene == scene; });
    auto at = std::find_if(overlay_layers_.begin(), overlay_layers_.end(),
                           [order](const OverlayLayer& l) { return l.order > order; });
    overlay_layers_.insert(at, OverlayLayer{scene, order, in_display_rect});
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
    coopa::ui::UiAudio::set_active(audio_->ui());
    coopa::ui::register_ui_audio_components();
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
    // Bus volumes: config.yaml's defaults, then whatever this player chose last time.
    UserSettings& us = UserSettings::instance();
    us.load();
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

void Engine::clear_scenes_() {
    if (audio_) audio_->stop_all();
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
    world::install_terrain_system(scene, ctx_.device(), ctx_.allocator(), assets_);
    // Order 90: bakes water surfaces and refreshes the buoyant-body list right before
    // Physics (100) steps, whose substep callback it drives -- see water_system.h's file doc.
    water::install_water_system(scene, &ctx_.device(), &ctx_.allocator(), &assets_)
        ->set_settings(water_settings_for_(pipeline_->render_config().water_quality));
    coopa::physx::system::install_physics_system(scene, scene_cfg.physics);
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
    // Order 370: after TransformResolve, so deformers stamp where they are drawn; reads the
    // weather (40) for the cover and the precipitation map -- see world/snow_system.h.
    world::install_snow_system(scene);

    // Activate TransformSystem before the first drain or render, so the world_matrix()
    // reads below are never asked to resolve a still-dirty transform; then block until
    // every load issued above has finished, so frame 0 sees a fully populated scene.
    coopa::scene::install_transform_system(scene);
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

void Engine::save_screenshot(const std::string& path, bool low_res) {
    coopa::gfx::util::save_image_png(capture_image(low_res), path);
    if (low_res) {
        std::cout << "[toyengine] Saved " << path << " (" << pipeline_->render_width()
                  << "x" << pipeline_->render_height() << ")\n";
    } else {
        std::cout << "[toyengine] Saved " << path << " (display resolution)\n";
    }
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
            drive_character_controllers_(scene_mgr_.get_active_scene());
            drive_ragdolls_(scene_mgr_.get_active_scene());
        }
        scene_mgr_.update(dt);
        if (overlay_scene_) overlay_scene_->update(dt);
        for (const OverlayLayer& l : overlay_layers_) l.scene->update(dt);
    }
    {
        CpuTimer t(prof, CpuScope::LateUpdate);
        if (scene_mgr_.has_scene()) {
            // Between update() and late_update(), and that window is the only correct slot:
            // world matrices are current only after UpdatePhase::TransformResolve (350) has
            // run inside update(), and a canvas needs its pointer position before
            // EventSystem::process() is dispatched from inside late_update() (400).
            drive_ui_canvases_(scene_mgr_.get_active_scene(), scene_ui_placement_);
        }
        if (overlay_scene_) drive_ui_canvases_(*overlay_scene_, std::nullopt);
        for (const OverlayLayer& l : overlay_layers_) drive_ui_canvases_(*l.scene, overlay_layer_placement_(l));
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
    update_audio_(dt);
    if (hooks_.post_late_update) hooks_.post_late_update(dt);

    if (scene_mgr_.has_scene()) {
        {
            CpuTimer t(prof, CpuScope::DynamicMeshes);
            upload_dynamic_meshes_(scene_mgr_.get_active_scene());
            gather_debug_lines_(scene_mgr_.get_active_scene());
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
        CpuTimer t(prof, CpuScope::Render);
        pipeline_->render(ctx_.renderer(), scene_mgr_.get_active_scene(), dt);
    }
    // toy::debug::watch()/text() lines last one frame.
    debug::GameLines::instance().clear();

    return !ctx_.should_close();
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

    if (tr.kind == SceneTransition::Kind::LoadingScreen && st.pending()) {
        if (covered && !st.loading_screen_shown) {
            st.loading_screen_shown = true;
            if (!tr.loading_screen.empty() && !transition_.show_loading_screen(tr.loading_screen)) {
                std::cerr << "[toyengine] Loading screen '" << tr.loading_screen << "' could not be shown\n";
            }
        }
        transition_.update_loading_screen(st.progress, scene_load_status_text(st.stage));
    }

    if (st.stage == SceneLoadStage::WaitingToSwap && covered && st.elapsed >= st.options.min_display_time) {
        activate_scene_load_(st);
    }

    if (covered_kind) transition_.set_fade(tr.color, st.alpha);
    if (st.stage == SceneLoadStage::FadingIn && st.alpha <= 0.0f) st.stage = SceneLoadStage::Done;
    if (st.finished() && st.alpha <= 0.0f) finish_transition_();
}

void Engine::activate_scene_load_(detail::SceneLoadState& st) {
    ctx_.wait_idle();
    transition_.hide_loading_screen();
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
    st.stage = st.options.transition.kind == SceneTransition::Kind::None ? SceneLoadStage::Done : SceneLoadStage::FadingIn;
    st.complete(*raw);
}

void Engine::apply_spawn_point_(coopa::scene::Scene& scene, const std::string& spawn_point) {
    if (spawn_point.empty()) return;
    coopa::scene::SceneObject* spawn = scene.find_object(spawn_point);
    auto* player = scene.find_first_component<scene::CharacterController>();
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

void Engine::sync_overlay_layers_() {
    std::vector<coopa::scene::Scene*> scenes;
    scenes.reserve(overlay_layers_.size());
    for (const OverlayLayer& l : overlay_layers_) scenes.push_back(l.scene);
    pipeline_->set_overlay_layers(std::move(scenes));
}

std::optional<Engine::ScreenUiPlacement> Engine::overlay_layer_placement_(const OverlayLayer& layer) {
    if (!layer.in_display_rect) return std::nullopt;
    const render::LetterboxRect rect = display_rect();
    if (rect.w == 0 || rect.h == 0) return std::nullopt;
    // At the display's scale, so the HUD keeps a physical size (and crisp text) on HiDPI.
    return ScreenUiPlacement{rect, /*input=*/false, std::max(1.0f, display_scale())};
}

void Engine::sync_live_profile_() {
    const bool want = debug_overlay_.full() && !profile_;
    if (want == (live_profile_ != nullptr)) return;
    if (want) {
        live_profile_ = std::make_unique<render::FrameProfile>();
        pipeline_->set_profiler(live_profile_.get());
    } else {
        pipeline_->set_profiler(nullptr);   // waits for the GPU, then drops its queries
        live_profile_.reset();
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

void Engine::sync_particle_render_state_(coopa::scene::Scene& scene) {
    particle_frame_.quads.clear();
    particle_frame_.meshes.clear();
    if (auto* sys = dynamic_cast<particles::ParticleSimulationSystem*>(scene.find_system("Particles"))) {
        using coopa::gfx::engine::components::CameraComponent;
        const CameraComponent* cam = CameraComponent::main();
        particles::ParticleView view;
        render::Frustum frustum{};
        if (cam) {
            view.camera_pos = cam->get_world_position();
            const float aspect = static_cast<float>(pipeline_->render_width()) /
                                 static_cast<float>(std::max(1u, pipeline_->render_height()));
            frustum = render::Frustum::from_matrix(cam->get_projection_matrix(aspect) * cam->get_view_matrix());
        }
        sys->collect_render(scene, view, [&](const glm::vec3& lo, const glm::vec3& hi) {
            if (!cam) return true;
            render::WorldBounds b;
            b.center = 0.5f * (lo + hi);
            b.extent = 0.5f * (hi - lo);
            return frustum.intersects(b);
        }, particle_frame_);
    }
    pipeline_->set_particle_state(particle_frame_);
}

fkyaml::node Engine::weather_node_(const fkyaml::node& settings) {
    if (settings.is_mapping() && settings.contains("weather")) return settings.at("weather");
    return fkyaml::node();
}

void Engine::WeatherRenderBase::capture(const render::ToyRenderConfig& c) {
    cloud_coverage = c.cloud_coverage;
    zenith = c.indirect.sky_zenith; horizon = c.indirect.sky_horizon; ground = c.indirect.sky_ground;
    ambient = c.indirect.ambient_intensity; sky = c.indirect.sky_intensity; exposure = c.exposure;
    fog_mode = c.fog_mode; fog_density = c.fog_density; fog_linear_start = c.fog_linear_start;
    fog_linear_end = c.fog_linear_end; fog_color = c.fog_color; fog_sky_blend = c.fog_sky_blend;
    fog_max_opacity = c.fog_max_opacity; fog_height_falloff = c.fog_height_falloff; fog_sun_amount = c.fog_sun_amount;
}

void Engine::WeatherRenderBase::restore(render::ToyRenderConfig& c) const {
    c.cloud_coverage = cloud_coverage;
    c.indirect.sky_zenith = zenith; c.indirect.sky_horizon = horizon; c.indirect.sky_ground = ground;
    c.indirect.ambient_intensity = ambient; c.indirect.sky_intensity = sky; c.exposure = exposure;
    c.fog_mode = fog_mode; c.fog_density = fog_density; c.fog_linear_start = fog_linear_start;
    c.fog_linear_end = fog_linear_end; c.fog_color = fog_color; c.fog_sky_blend = fog_sky_blend;
    c.fog_max_opacity = fog_max_opacity; c.fog_height_falloff = fog_height_falloff; c.fog_sun_amount = fog_sun_amount;
}

void Engine::sync_weather_render_state_(coopa::scene::Scene& scene) {
    weather::WeatherSystem* w = weather::find(scene);
    if (!w || !w->enabled() || !w->state().enabled) { restore_weather_atmosphere_(); return; }
    render::ToyRenderConfig& c = pipeline_->render_config_mut();
    if (!weather_applied_) { weather_base_.capture(c); weather_applied_ = true; }
    const weather::Atmosphere& a = w->atmosphere();
    c.indirect.sky_zenith = a.sky_zenith;
    c.indirect.sky_horizon = a.sky_horizon;
    c.indirect.sky_ground = a.sky_ground;
    c.indirect.ambient_intensity = a.ambient_intensity;
    c.indirect.sky_intensity = a.sky_intensity;
    c.fog_mode = 1;   // exponential height fog (the condition's falloff thins it upward)
    c.fog_density = a.fog_density;
    c.fog_color = a.fog_color;
    c.fog_sky_blend = a.fog_sky_blend;
    c.fog_max_opacity = a.fog_max_opacity;
    c.fog_height_falloff = a.fog_height_falloff;
    c.fog_sun_amount = a.fog_sun_amount;
    c.exposure = weather_base_.exposure * a.exposure_scale;   // scaled, not owned: the row stays editable
    c.cloud_coverage = std::clamp(w->state().cloud_cover, 0.0f, 1.0f);
}

void Engine::sync_sky_render_state_(coopa::scene::Scene& scene, float dt) {
    using coopa::gfx::engine::components::DirectionalLightComponent;
    render::ToyRenderConfig& c = pipeline_->render_config_mut();
    weather::WeatherSystem* w = weather::find(scene);
    const bool physical = c.sky_model == "physical";
    if (w) w->set_physical_sky(physical);
    const bool weather_live = w && w->enabled() && w->state().enabled;

    // Cloud drift and evolution (either sky model).
    glm::vec2 wind_dir(1.0f, 0.0f);
    if (weather_live && glm::length(glm::vec2(w->state().wind)) > 0.05f) wind_dir = glm::normalize(glm::vec2(w->state().wind));
    cloud_offset_ += glm::dvec2(wind_dir) * static_cast<double>(c.cloud_wind_speed * std::max(dt, 0.0f));
    // The flat clouds' morph: a phase through the shape volume's (repeating) depth, at the
    // layer's evolve rate -- accumulated, so changing the rate never jumps the shapes.
    cloud_flat_phase_ = std::fmod(cloud_flat_phase_ + 0.0015 * std::max(c.flat_cloud_evolve, 0.0f) * std::max(dt, 0.0f), 1.0);
    sky_time_ += std::max(dt, 0.0f);

    render::SkyFrameState st;
    if (!physical) {
        restore_sky_colours_();
        pipeline_->set_sky_state(st);
        sync_cloud_state_(scene, nullptr);
        return;
    }
    if (weather_live && sky_applied_) {
        // The weather started over our colours and captured them as "the config's": hand it
        // the real ones, and let its base own them from here on.
        weather_base_.zenith = sky_base_[0]; weather_base_.horizon = sky_base_[1]; weather_base_.ground = sky_base_[2];
        sky_applied_ = false;
    } else if (!weather_live && !sky_applied_) {
        sky_base_ = {c.indirect.sky_zenith, c.indirect.sky_horizon, c.indirect.sky_ground};
        sky_applied_ = true;
    }

    // The scene's sun: the same light the renderer picks (the first active one).
    DirectionalLightComponent* light = nullptr;
    for (DirectionalLightComponent* l : scene.get_components<DirectionalLightComponent>()) {
        if (l->owner && l->owner->active()) { light = l; break; }
    }
    glm::vec3 sun_to = glm::normalize(glm::vec3(0.4f, 0.3f, 0.85f));
    if (weather_live) sun_to = w->state().sun_direction;
    else if (light && glm::length(light->direction) > 1e-6f) sun_to = -glm::normalize(light->direction);
    const float coverage = std::clamp(c.cloud_coverage, 0.0f, 1.0f);
    // Sun illuminance the sky is scaled by: the clear-sky sun (the weather's, else the light's own).
    const float sun_ref = weather_live ? w->settings().sun_intensity : (light ? std::max(light->intensity, 0.0f) : 1.2f);

    sky_atmosphere_.set_media(render::SkyAtmosphereMedia::earth(c.atmosphere_density, c.ozone));
    glm::vec3 cam_pos(0.0f);
    if (auto* cam = coopa::gfx::engine::components::CameraComponent::main()) cam_pos = cam->get_world_position();
    const float view_r = sky_atmosphere_.view_radius(cam_pos.z);
    const float K = k_sky_gain * sun_ref;
    const weather::SkyGradient g = sky_atmosphere_.gradient(sun_to, k_moon_ratio, view_r);
    const glm::vec3 floor_col = glm::vec3(0.55f, 0.75f, 1.3f) * 1e-3f * K * 0.3f;
    glm::vec3 zenith = g.zenith * K + floor_col, horizon = g.horizon * K + floor_col, ground = g.ground * K + floor_col * 0.5f;
    if (c.clouds && coverage > 0.0f) {
        // Cloud cover greys the dome toward the overcast's flat light, a little darker.
        auto overcast = [&](glm::vec3 col) {
            const float lum = glm::dot(col, glm::vec3(0.2126f, 0.7152f, 0.0722f));
            return glm::mix(col, glm::vec3(lum * (1.0f - 0.45f * coverage)), coverage * 0.85f);
        };
        const glm::vec3 mean = (zenith + horizon) * 0.5f;
        zenith = overcast(glm::mix(zenith, mean, coverage * 0.6f));
        horizon = overcast(glm::mix(horizon, mean, coverage * 0.6f));
        ground = overcast(ground);
    }
    c.indirect.sky_zenith = zenith;
    c.indirect.sky_horizon = horizon;
    c.indirect.sky_ground = ground;

    // The light's colour through the air: transmittance toward it, relative to straight up
    // (so a high sun stays as authored and a low one reddens and fades).
    const glm::vec3 light_to = light && glm::length(light->direction) > 1e-6f ? -glm::normalize(light->direction) : sun_to;
    const glm::vec3 t_up = glm::max(sky_atmosphere_.transmittance(view_r, 1.0f), glm::vec3(1e-4f));
    glm::vec3 tint = sky_atmosphere_.transmittance_toward(view_r, light_to) / t_up;
    // Without shadows, the cover dims the sun uniformly (the weather's condition dims its own).
    if (c.clouds && !c.cloud_shadows && !weather_live) tint *= glm::mix(1.0f, 0.3f, coverage * coverage);
    st.light_tint = glm::min(tint, glm::vec3(1.0f));

    st.active = true;
    st.media = sky_atmosphere_.media();
    st.sun_to = sun_to;
    st.moon_to = -sun_to;
    st.sky_illuminance = K;
    st.moon_ratio = k_moon_ratio;
    st.sun_disc_radiance = c.sun_disc_size > 0.0f ? K * 40.0f : 0.0f;
    st.moon_disc_radiance = c.moon_disc_size > 0.0f ? K * k_moon_ratio * 60.0f : 0.0f;
    st.sun_disc_cos = std::cos(glm::radians(std::max(c.sun_disc_size, 0.0f) * 0.5f));
    st.moon_disc_cos = std::cos(glm::radians(std::max(c.moon_disc_size, 0.0f) * 0.5f));
    const float dark = std::clamp((-0.02f - sun_to.z) / 0.16f, 0.0f, 1.0f);
    st.star_visibility = c.sky_stars ? dark * dark * (3.0f - 2.0f * dark) * (c.clouds ? 1.0f - 0.5f * coverage : 1.0f) : 0.0f;
    st.night_floor = K * 0.3f;

    st.time = std::fmod(sky_time_, 3600.0f);
    pipeline_->set_sky_state(st);
    sync_cloud_state_(scene, &st);
}

void Engine::sync_cloud_state_(coopa::scene::Scene& scene, const render::SkyFrameState* sky) {
    using coopa::gfx::engine::components::DirectionalLightComponent;
    const render::ToyRenderConfig& c = pipeline_->render_config();
    render::CloudFrameState cs;
    cs.active = c.clouds;
    cs.type = c.cloud_type == "flat" ? render::CloudType::Flat : render::CloudType::Volumetric;
    cs.coverage = std::clamp(c.cloud_coverage, 0.0f, 1.0f);
    cs.altitude = c.cloud_altitude;
    cs.thickness = std::max(c.cloud_thickness, 0.1f);
    cs.density = std::max(c.cloud_density, 0.0f);
    cs.scale = std::clamp(c.cloud_scale, 1e-4f, 100.0f);
    cs.offset = cloud_offset_;
    cs.time = std::fmod(sky_time_, 3600.0f);
    cs.shadows = c.cloud_shadows;
    cs.shadow_strength = std::clamp(c.cloud_shadow_strength, 0.0f, 1.0f);
    cs.shadow_distance = std::max(c.cloud_shadow_distance, 1.0f);
    cs.flat_size = std::max(c.flat_cloud_size, 0.5f);
    cs.flat_opacity = std::clamp(c.flat_cloud_opacity, 0.0f, 1.0f);
    cs.flat_light_bands = std::max(c.flat_cloud_light_bands, 0.0f);
    cs.flat_outline = std::clamp(c.flat_cloud_outline, 0.0f, 1.0f);
    cs.flat_turbulence = std::clamp(c.flat_cloud_turbulence, 0.0f, 4.0f);
    cs.flat_evolve = std::max(c.flat_cloud_evolve, 0.0f);
    cs.flat_phase = static_cast<float>(cloud_flat_phase_);

    glm::vec3 cam_pos(0.0f);
    if (auto* cam = coopa::gfx::engine::components::CameraComponent::main()) cam_pos = cam->get_world_position();
    if (c.cloud_camera_fade) {
        const float above = cam_pos.z - (cs.altitude + cs.thickness);
        const float end = std::max(c.cloud_fade_end, c.cloud_fade_start + 0.01f);
        const float f = std::clamp((above - c.cloud_fade_start) / (end - c.cloud_fade_start), 0.0f, 1.0f);
        cs.fade = f * f * (3.0f - 2.0f * f);
    }

    if (sky) {
        // Clouds are lit by the sun until it is well below the horizon, then by the moon.
        const float K = sky->sky_illuminance;
        const glm::vec3 sun_to = sky->sun_to;
        cs.atmosphere_lut = true;
        if (sun_to.z > -0.12f) {
            cs.light_to = sun_to;
            // A little softer while the sun is low, so a sunset's lit cloud deck does not drive
            // the exposure meter so hard that the ground (lit at a grazing angle) goes black.
            const float low = std::clamp(sun_to.z / 0.3f, 0.0f, 1.0f);
            cs.light_color = glm::vec3(K * (0.6f + 0.4f * low));
        } else {
            cs.light_to = -sun_to;
            cs.light_color = glm::vec3(0.75f, 0.85f, 1.0f) * K * k_moon_ratio * 0.5f;
        }
    } else {
        // The gradient sky: the scene's light (the one the renderer picks -- the first active)
        // as it lights every surface, with no sky gain: the clouds then sit against the
        // gradient's own colours about as a white surface would, instead of the physical
        // sky's brighter-than-life dome.
        DirectionalLightComponent* light = nullptr;
        for (DirectionalLightComponent* l : scene.get_components<DirectionalLightComponent>()) {
            if (l->owner && l->owner->active()) { light = l; break; }
        }
        if (light && glm::length(light->direction) > 1e-6f) {
            cs.light_to = -glm::normalize(light->direction);
            cs.light_color = light->color * std::max(light->intensity, 0.0f);
        } else {
            cs.light_to = glm::normalize(glm::vec3(0.4f, 0.3f, 0.85f));
            cs.light_color = glm::vec3(0.0f);
        }
    }
    pipeline_->set_cloud_state(cs);
}

void Engine::sync_surface_state_(coopa::scene::Scene& scene) {
    render::SurfaceFrameState st;
    const render::ToyRenderConfig& c = pipeline_->render_config();
    weather::WeatherSystem* w = weather::find(scene);
    const bool live = w && w->enabled() && w->state().enabled;
    if (live) {
        const weather::WeatherState& ws = w->state();
        st.snow_cover = ws.snow_cover;
        st.snow_depth = w->settings().snow_max_depth;
        st.snow_patch_hard = w->settings().snow_patch_hard;
        st.snow_patch_size = w->settings().snow_patch_size;
        st.wetness = ws.wetness;
        st.wind = ws.wind;
        if (const auto& field = w->ground_probe().field()) {
            // Aliasing pointer: the heights live as long as the field the renderer holds.
            // The sky layer (moving bodies looked through) where the probe made one.
            st.occl_heights = std::shared_ptr<const std::vector<float>>(
                field, field->sky_heights.size() == field->heights.size() ? &field->sky_heights : &field->heights);
            st.occl_origin = field->origin;
            st.occl_cell = field->cell;
            st.occl_nx = field->nx;
            st.occl_ny = field->ny;
            st.occl_fallback = field->fallback;
        }
    }
    if (c.snow_cover_override >= 0.0f) st.snow_cover = std::min(c.snow_cover_override, 1.0f);
    if (const world::SnowSystem* snow = world::find_snow(scene); snow && st.snow_cover > 0.0f) {
        const world::SnowField& f = snow->field();
        if (f.focused()) {
            st.trench_words = f.words().data();
            st.trench_n = f.n();
            st.trench_cell = f.cell();
            st.trench_window = f.window();
            st.trench_scale = f.scale();
            st.trench_version = f.version();
        }
    }
    pipeline_->set_surface_state(std::move(st));
}

void Engine::restore_sky_colours_() {
    if (!sky_applied_) return;
    render::ToyRenderConfig& c = pipeline_->render_config_mut();
    c.indirect.sky_zenith = sky_base_[0]; c.indirect.sky_horizon = sky_base_[1]; c.indirect.sky_ground = sky_base_[2];
    sky_applied_ = false;
}

void Engine::restore_weather_atmosphere_() {
    if (!weather_applied_) return;
    weather_base_.restore(pipeline_->render_config_mut());
    weather_applied_ = false;
}

water::WaterSettings Engine::water_settings_for_(render::RenderQuality q) {
    switch (q) {
        case render::RenderQuality::Low:    return water::WaterSettings::from_quality(water::WaterQuality::Low);
        case render::RenderQuality::Medium: return water::WaterSettings::from_quality(water::WaterQuality::Medium);
        case render::RenderQuality::Ultra:  return water::WaterSettings::from_quality(water::WaterQuality::Ultra);
        case render::RenderQuality::High:   break;
    }
    return water::WaterSettings::from_quality(water::WaterQuality::High);
}

void Engine::sync_water_render_state_(coopa::scene::Scene& scene) {
    render::WaterFrameState state;
    auto* water = dynamic_cast<water::WaterSystem*>(scene.find_system("Water"));
    if (water) {
        // water_quality is live: a changed tier reaches the system for its next update.
        const render::RenderQuality q = pipeline_->render_config().water_quality;
        if (water->settings().quality != water_settings_for_(q).quality) water->set_settings(water_settings_for_(q));
        const water::WaterSettings& ws = water->settings();
        state.ripple_layers   = ws.ripple_layers;
        state.detail_distance = ws.detail_distance;
        state.ripple_range    = ws.ripple_range;

        // Hooks between Scene::update() and here (the editor applying its edits) may have
        // destroyed or rebuilt water objects since the system cached its list.
        water->refresh_bodies(scene);
        const float now = water->time();
        state.time = water->render_time();
        auto* cam = coopa::gfx::engine::components::CameraComponent::main();
        const bool has_eye = cam && cam->owner;
        const glm::vec3 eye = has_eye ? glm::vec3(cam->owner->get_transform()->transform().get_world_matrix()[3])
                                      : glm::vec3(0.0f);
        // Rings within the draw range, nearest first, at most the tier's cap -- then handed
        // over oldest first, the order the pipeline expects.
        std::vector<std::pair<float, std::size_t>> near;
        near.reserve(water->ripples().size());
        for (std::size_t i = 0; i < water->ripples().size(); ++i) {
            const float d = has_eye ? glm::distance(glm::vec2(eye), water->ripples()[i].position) : 0.0f;
            if (d <= ws.ripple_range) near.push_back({d, i});
        }
        if (near.size() > ws.max_ripples) {
            std::partial_sort(near.begin(), near.begin() + static_cast<std::ptrdiff_t>(ws.max_ripples), near.end());
            near.resize(ws.max_ripples);
        }
        std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        state.ripples.reserve(near.size());
        for (const auto& [d, i] : near) {
            const auto& r = water->ripples()[i];
            state.ripples.push_back({r.position, now - r.birth, r.strength, r.radius});
        }
        if (has_eye) {
            const water::WaterSystem::UnderwaterInfo info = water->underwater_at(eye);
            // Also just ABOVE the surface (within half a metre): the bottom of the near plane
            // can already be under water, and the pass decides per pixel which side it is on
            // -- that is what splits the image at the waterline.
            const bool near_surface = info.body && info.depth > -0.5f && info.depth <= 0.0f;
            if ((info.underwater || near_surface) && info.body) {
                state.underwater    = true;
                state.surface_level = info.surface_height;
                state.fog_color     = info.body->underwater_color;
                state.visibility    = info.body->underwater_visibility;
                state.absorption    = info.body->underwater_absorption;
                state.caustics      = info.body->caustics;
            }
        }
    }
    pipeline_->set_water_state(std::move(state));
}

void Engine::update_fill_extent_() {
    const render::ToyRenderConfig& cfg = pipeline_->render_config();
    if (cfg.resolution_mode != "fill") return;
    const VkExtent2D sc = ctx_.swapchain().extent();
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

coopa::gfx::app::ContextConfig Engine::make_context_config_(const AppConfig& config) {
    coopa::gfx::app::ContextConfig cc;
    cc.title      = config.window.title;
    cc.width      = config.window.width;
    cc.height     = config.window.height;
    cc.resizable  = true;
    cc.vsync      = config.window.vsync;
    cc.visible    = config.window.visible;
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
    cc->mouse_delta = ctx_.input().cursor_delta();
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

void Engine::drive_character_controllers_(coopa::scene::Scene& scene) {
    std::vector<scene::CharacterController*> characters = scene.get_components<scene::CharacterController>();
    if (characters.empty()) return;

    float yaw = 0.0f;
    if (auto* cam = coopa::gfx::engine::components::CameraComponent::main()) {
        const glm::mat4 world = glm::inverse(cam->get_view_matrix());
        glm::vec3 fwd = -glm::vec3(world[2]);            // cameras look down local -Z
        if (std::abs(fwd.z) > 0.99f) fwd = glm::vec3(world[1]); // looking straight down: screen-up
        if (glm::length(glm::vec2(fwd.x, fwd.y)) > 1e-4f) yaw = glm::degrees(std::atan2(-fwd.x, fwd.y));
    }
    const bool blocked = input_blocked_();
    const glm::vec2 move = blocked ? glm::vec2(0.0f) : input_.vector("move", ctx_.input());
    const bool jump = !blocked && input_.is_pressed("jump", ctx_.input());
    const bool sprint = !blocked && input_.is_down("sprint", ctx_.input());
    for (scene::CharacterController* cc : characters) {
        cc->move_input = move;
        cc->move_basis_yaw_deg = yaw;
        cc->jump = cc->jump || jump;
        cc->sprint = sprint;
    }
}

void Engine::drive_ragdolls_(coopa::scene::Scene& scene) {
    if (input_blocked_() || !input_.is_pressed("ragdoll", ctx_.input())) return;
    for (scene::Ragdoll* r : scene.get_components<scene::Ragdoll>()) {
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

void Engine::drive_ui_canvases_(coopa::scene::Scene& scene, const std::optional<ScreenUiPlacement>& placement) {
    std::vector<coopa::ui::CanvasComponent*> canvases = coopa::ui::collect_canvases(scene);
    if (canvases.empty()) return;
    // A scene that is drawn but not simulating (the editor's edit mode) never runs a
    // canvas's late_update(), which is where layout and emit happen -- so without this
    // every HUD and nameplate would be invisible while editing.
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
            // Cursor positions arrive in screen points; the canvas is sized in framebuffer
            // pixels. They differ on HiDPI displays.
            canvas->set_input(ctx_.input(), cursor_scale());
            if (preview) canvas->preview_refresh();
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
        if (preview) canvas->preview_refresh();
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

void Engine::apply_app_icon_() {
    static const int kSizes[] = {16, 32, 48, 64, 128, 256, 512};
    std::vector<std::vector<uint8_t>> pixels;
    std::vector<coopa::gfx::presentation::Window::IconImage> images;
    for (int s : kSizes) pixels.push_back(rasterize_logo(s));
    for (size_t i = 0; i < pixels.size(); ++i) images.push_back({kSizes[i], kSizes[i], pixels[i].data()});
    ctx_.window().set_icon(images);
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
    while (assets_.pending_load_count() > 0) {
        assets_.update(0.0f);
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

std::string Engine::profile_path_from_env_() {
    const char* v = debug_env("PROFILE");
    if (!v || !*v || std::string(v) == "0") return {};
    const std::string s(v);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".csv") == 0) return s;
    return "output/profile.csv";
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
        /* frag  */ "",  // reuses stock gbuffer.frag -- CUTOUT needs no fragment override
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
        /* name  */ "terrain",
        /* domain */ coopa::gfx::pipeline::SurfaceShaderDomain::Opaque,
        /* vert  */ "",  // stock gbuffer.vert -- only the UV decode differs
        /* frag  */ "terrain.frag", // tile-space UVs from the greedy chunk mesher
        /* shadow_vert */ "", // stock: an opaque caster never reads UVs in the shadow pass
        /* shadow_frag */ "",
        /* shadow_cube_vert */ "",
        /* shadow_cube_frag */ "",
        /* cull */ coopa::gfx::CullMode::Back, // closed columns, like any opaque solid
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

    return rc;
}

} // namespace core
} // namespace toy
