/**
 * @file engine_sub_scene.cpp
 * @brief Sub-scene views: loading a second scene, giving it its own offscreen pipeline, and
 *        keeping it simulated and drawn inside the host's frame.
 *
 * ## Why the loading happens here and not in the component
 *
 * Loading a scene calls `prepare_scene_()`, which installs thirty-odd systems, drains assets and
 * resyncs the clock. Doing that from inside a component's `start()` would be reentrant
 * scene-manager mutation in the middle of a scene build. So the component only declares, and
 * `ensure_sub_views_()` -- called from `tick()`, on the main thread, outside any scene walk --
 * does the work on the following frame. That is the same deferral `SceneLink` uses for scene
 * changes a component asks for.
 *
 * ## The main-camera singleton
 *
 * `CameraComponent::start()` claims the process-global main camera when its object declares
 * `main: true`, or when nothing else has claimed it. A sub-scene authored to run standalone
 * declares it, which would evict the host's camera the moment the sub-scene started. So the
 * sub-scene is built UNSTARTED, every camera's claim is cleared (remembering which one wanted
 * it, since that is the camera to render from), and only then is it started.
 */

#include <toyengine/core/engine.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <utility>

#include <coopa/scene/scene_loader.h>
#include <coopa/scene/scene_object.h>
#include <gfxcoopa/engine/components/camera_component.h>
#include <toyengine/render/toy_render_pipeline.h>
#include <toyengine/scene/view_camera.h>

namespace toy::core {

using coopa::gfx::engine::components::CameraComponent;

namespace {

/// A `scene:` key as a document path. A bare name is a scene FOLDER, exactly as the SCENE
/// environment variable and a project's configured default scene spell it, so
/// `ui/map_world_demo` means `assets/scenes/ui/map_world_demo/scene.yaml`. An explicit
/// document path is taken as given.
std::string scene_doc_path(const std::string& ref) {
    return coopa::yaml::is_document_ext(ref) ? ref : "assets/scenes/" + ref + "/scene.yaml";
}

}  // namespace

render::ToyRenderPipeline* Engine::pipeline_for_scene_(const coopa::scene::Scene* scene) const {
    for (const SubSceneView& v : sub_views_) {
        if (v.scene == scene && v.pipeline) return v.pipeline.get();
    }
    return pipeline_.get();
}

render::SubScene* Engine::find_sub_scene_component_(const SubSceneView& view) const {
    if (view.host == nullptr) return nullptr;
    for (render::SubScene* c : view.host->get_components<render::SubScene>()) {
        if (c->view_name == view.view_name && c->scene_ref == view.scene_ref) return c;
    }
    return nullptr;
}

render::ToyRenderConfig Engine::sub_view_config_(const render::SubScene& c,
                                                 const coopa::scene::Scene& sub) const {
    // Built from the sub-scene's OWN config -- scene_config() folds in its settings.render --
    // so a scene authored to stand alone keeps its look when drawn into a texture.
    render::ToyRenderConfig cfg = make_render_config_(scene_config(sub), options_.project_root);
    // ...but the shader library and the registered surface shaders come from the host's
    // resolved config: a sub-view draws the same materials and must resolve the same names.
    cfg.shaders         = pipeline_->render_config().shaders;
    cfg.surface_shaders = pipeline_->render_config().surface_shaders;

    // --- What a view drawn into a texture cannot or should not do ---
    cfg.resolution_mode = "fixed";          // "fill"/"divisor" are window concepts
    cfg.render_width    = std::max(1u, c.width);
    cfg.render_height   = std::max(1u, c.height);
    cfg.fill_aspect     = 0.0f;
    // A sub-view is geometry and lighting: its own screen UI is not drawn, and a frosted
    // backdrop inside a panel inside a frosted panel is nonsense. The two move together
    // because the backdrop pass is only built when screen UI is.
    cfg.screen_ui_enabled        = false;
    cfg.ui_backdrop_blur_enabled = false;
    cfg.ui_scene_blur            = 0.0f;

    // The shadow atlases are sized from the CONFIG, not from the view extent, so at stock
    // settings a 512x512 view would allocate as much shadow memory as a full-window one --
    // about 256 MiB of it. Scaled down here unless the sub-scene asked for more itself.
    cfg.shadow_map_resolution = std::min(cfg.shadow_map_resolution, 1024u);
    cfg.shadow_cascades       = std::min(cfg.shadow_cascades, 2u);
    cfg.local_shadow_atlas_resolution = std::min(cfg.local_shadow_atlas_resolution, 1024u);
    return cfg;
}

void Engine::apply_sub_views_to_pipeline_() {
    if (!pipeline_) return;
    std::vector<render::ToyRenderPipeline*> views;
    views.reserve(sub_views_.size());
    for (const SubSceneView& v : sub_views_) {
        if (v.pipeline) views.push_back(v.pipeline.get());
    }
    // Re-applied every frame rather than once at construction: rebuild_pipeline() replaces
    // pipeline_ wholesale and carries over only what it names, so a render setting changed at
    // runtime would otherwise silently stop every sub-view from being recorded.
    pipeline_->set_sub_views(std::move(views));
}

void Engine::drop_sub_view_(SubSceneView& view) {
    // The host's screen-UI pass caches a descriptor set per texture it ever drew and never
    // evicts, so the registration has to go before the image does -- after the device is idle,
    // because a frame that drew it may still be in flight.
    if (view.pipeline && pipeline_) {
        ctx_.wait_idle();
        pipeline_->unregister_screen_ui_texture(view.pipeline->output_view());
    }
    view.pipeline.reset();
    if (view.scene != nullptr) {
        pinned_active_.erase(view.scene);
        remove_scene(view.scene);
        view.scene = nullptr;
    }
}

void Engine::ensure_sub_views_() {
    if (!pipeline_ || !scene_mgr_.has_scene()) return;
    coopa::scene::Scene& host = scene_mgr_.get_active_scene();

    // --- Drop views whose declaration is gone (component deleted, or host scene replaced) ---
    // The host check comes FIRST and short-circuits, so a host that has since been removed is
    // never dereferenced by find_sub_scene_component_() below.
    for (SubSceneView& v : sub_views_) {
        if (v.host != &host || find_sub_scene_component_(v) == nullptr) drop_sub_view_(v);
    }
    const size_t before = sub_views_.size();
    sub_views_.erase(std::remove_if(sub_views_.begin(), sub_views_.end(),
                                    [](const SubSceneView& v) { return v.scene == nullptr; }),
                     sub_views_.end());

    // --- Load any declaration that has no view yet ---
    bool added = false;
    for (render::SubScene* c : host.get_components<render::SubScene>()) {
        if (c->scene_ref.empty()) continue;
        const bool have = std::any_of(sub_views_.begin(), sub_views_.end(),
                                      [&](const SubSceneView& v) {
                                          return v.host == &host && v.view_name == c->view_name &&
                                                 v.scene_ref == c->scene_ref;
                                      });
        if (have) continue;

        // Remembered failures are not retried: the load throws, and retrying every frame would
        // turn one typo'd path into a per-frame exception rather than a single complaint.
        const std::string key = c->view_name + '\0' + c->scene_ref;
        if (failed_sub_views_.count(key) != 0) continue;

        const std::string resolved = resolve_scene_path_(scene_doc_path(c->scene_ref));
        coopa::scene::Scene built;
        try {
            built = coopa::scene::SceneLoader::load(resolved, {.start = false});
        } catch (const std::exception& e) {
            // A sub-view is an ADDITION to a scene. One that cannot load must cost its own
            // panel and nothing else -- taking the whole game down over it would make a
            // mistyped path indistinguishable from an engine fault.
            failed_sub_views_.insert(key);
            std::cerr << "[toyengine] SubScene '" << c->scene_ref << "' failed to load ("
                      << e.what() << "); the view is skipped.\n";
            continue;
        }

        // Clear every main-camera claim BEFORE starting, remembering which camera wanted it:
        // that is the one to render from when the component names none.
        std::string declared_main;
        for (CameraComponent* cam : built.get_components<CameraComponent>()) {
            if (cam->is_main) {
                cam->is_main = false;
                if (declared_main.empty() && cam->owner != nullptr) declared_main = cam->owner->name();
            }
        }
        built.start();

        coopa::scene::Scene* raw =
            scene_mgr_.add_scene(std::make_unique<coopa::scene::Scene>(std::move(built)));
        scene_settings_[raw] = read_scene_settings_(resolved);
        scene_paths_[raw]    = resolved;

        SubSceneView view;
        view.host       = &host;
        view.view_name  = c->view_name;
        view.scene_ref  = c->scene_ref;
        view.scene      = raw;
        // After the scene is registered, because its config is read through scene_config().
        view.pipeline   = std::make_unique<render::ToyRenderPipeline>(
            ctx_.device(), ctx_.allocator(), ctx_.command_pool(), sub_view_config_(*c, *raw),
            coopa::gfx::Extent2D{std::max(1u, c->width), std::max(1u, c->height)});
        view.pipeline->set_job_engine(&jobs_);
        view.pipeline->set_parallel_threshold(config_.jobs.parallel_threshold);

        // prepare_scene_ installs the systems; done with the view's pipeline reachable through
        // pipeline_for_scene_(), so anything it binds for later targets this view.
        sub_views_.push_back(std::move(view));
        prepare_scene_(*raw);
        // Simulated alongside the host rather than instead of it, and exempt from the
        // exclusivity activate_scene() otherwise enforces.
        scene_mgr_.set_scene_active(raw, true);
        pinned_active_.insert(raw);
        raw->set_simulating(c->simulate_in_edit_mode ? true : !edit_mode_);
        if (declared_main.empty() && c->camera.empty()) {
            std::cerr << "[toyengine] SubScene '" << c->scene_ref
                      << "' has no camera declaring `main: true` and names none; its first "
                         "camera is used.\n";
        }
        if (c->camera.empty()) c->camera = declared_main;
        added = true;
    }
    if (added || sub_views_.size() != before) apply_sub_views_to_pipeline_();
}

render::SubScene* Engine::pointer_owner_() const {
    for (const SubSceneView& v : sub_views_) {
        if (v.scene == nullptr) continue;
        render::SubScene* c = find_sub_scene_component_(v);
        // panel_reported gates this: a view nobody draws has no rect, and must never take the
        // pointer away from the host just because it exists.
        if (c != nullptr && c->input == render::SubSceneInput::Hover && c->panel_reported &&
            c->panel_hovered) {
            return c;
        }
    }
    return nullptr;
}

const coopa::input::Input& Engine::sub_view_input_(const render::SubScene& view) {
    sub_input_ = ctx_.input();
    const glm::vec4 r = view.panel_rect;
    if (r.z > 0.0f && r.w > 0.0f) {
        // Window pixels -> the view's own pixels. A view drawn into a panel smaller than its
        // extent magnifies movement, and larger shrinks it, so the delta is scaled by the same
        // ratio as the position -- otherwise a drag across the panel would travel the wrong
        // distance in the scene.
        const glm::vec2 scale(static_cast<float>(view.width) / r.z,
                              static_cast<float>(view.height) / r.w);
        const glm::vec2 local = (ctx_.input().cursor_position() - glm::vec2(r.x, r.y)) * scale;
        // set_cursor() rather than push_cursor_position(): the latter would measure the delta
        // across the jump between the window's coordinate space and this view's.
        sub_input_.set_cursor(local, ctx_.input().cursor_delta() * scale);
    }
    return sub_input_;
}

void Engine::drive_scene_input_(coopa::scene::Scene& scene, const coopa::input::Input& input,
                                bool routed_away) {
    const coopa::input::Input* prev_input = active_input_;
    const bool prev_routed = input_routed_away_;
    active_input_       = &input;
    input_routed_away_  = routed_away;

    drive_camera_controller_(scene);
    drive_kinematic_controllers_(scene);
    drive_free_movers_(scene);
    drive_fire_igniters_(scene);
    drive_damage_cursors_(scene);
    drive_turntables_(scene);
    drive_terrain_diggers_(scene);
    drive_character_controllers_(scene);
    drive_vehicles_(scene);
    drive_ragdolls_(scene);

    active_input_      = prev_input;
    input_routed_away_ = prev_routed;
}

void Engine::sync_sub_views_(float dt, render::FrameProfile* prof) {
    // Re-applied unconditionally: cheap, and it is what survives a pipeline rebuild.
    apply_sub_views_to_pipeline_();
    for (SubSceneView& v : sub_views_) {
        if (!v.pipeline || v.scene == nullptr) continue;
        render::SubScene* c = find_sub_scene_component_(v);
        if (c == nullptr) continue;

        // The camera to render from: the one the component names, else the scene's own.
        const CameraComponent* cam = nullptr;
        if (!c->camera.empty()) {
            if (coopa::scene::SceneObject* obj = v.scene->find_object(c->camera)) {
                cam = obj->get_component<CameraComponent>();
            }
        }
        if (cam == nullptr) cam = v.scene->find_first_component<CameraComponent>();
        if (cam == nullptr) continue;   // nothing to render from; leave last frame's image

        // A host's UI zoom writes ortho_size; the projection is rebuilt from it every frame.
        if (c->ortho_size > 0.0f) {
            const_cast<CameraComponent*>(cam)->orthographic_size = c->ortho_size;
        }

        sync_view_(*v.scene, *v.pipeline, v.sync, cam, dt, prof);
        v.pipeline->prepare_view(ctx_.renderer(), *v.scene, dt, cam);
        c->texture = v.pipeline->output_view();
        c->ready   = c->texture.valid();
    }
}

}  // namespace toy::core
