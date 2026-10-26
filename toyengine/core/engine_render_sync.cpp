/**
 * @file engine_render_sync.cpp
 * @brief Scene state -> render config: the per-frame sync_* family.
 *
 * Each sync_*_render_state_() translates what the scene says (weather, sky, clouds, water,
 * surfaces, particles) into the live ToyRenderConfig, and the restore_* pair puts the config's
 * own values back when the scene stops driving them. Order-sensitive: they run from tick() in
 * the sequence Engine::tick() establishes.
 *
 * A sibling translation unit of engine.cpp -- same Engine class, same header
 * (toyengine/core/engine.h), split by concern.
 */

#include <toyengine/core/engine.h>

#include <toyengine/env/env_system.h>

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

void Engine::sync_overlay_layers_() {
    std::vector<coopa::scene::Scene*> scenes;
    scenes.reserve(overlay_layers_.size());
    for (const OverlayLayer& l : overlay_layers_) scenes.push_back(l.scene);
    pipeline_->set_overlay_layers(std::move(scenes));
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

    // Cloud drift and evolution (either sky model). With a live weather the clouds go the
    // wind's way, and cloud_wind_speed is their speed in a 5 m/s ground wind: a calm day
    // barely moves them (a quarter), a gale races them (up to 4x -- winds aloft are stronger).
    glm::vec2 wind_dir(1.0f, 0.0f);
    float cloud_speed = c.cloud_wind_speed;
    if (weather_live) {
        const glm::vec2 wind(w->state().wind);
        const float ws = glm::length(wind);
        if (ws > 0.05f) wind_dir = wind / ws;
        cloud_speed *= std::clamp(ws / 5.0f, 0.25f, 4.0f);
    }
    cloud_offset_ += glm::dvec2(wind_dir) * static_cast<double>(cloud_speed * std::max(dt, 0.0f));
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
        st.wind_gust = ws.wind_gust;
        st.wind_time = ws.wind_time;
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
    // Heightmap terrains: their slots' numbers and textures (G-buffer set 3).
    if (const world::HeightmapTerrainSystem* hts = world::find_heightmap_terrain_system(scene)) {
        st.terrains = hts->frame_bindings();
    }
    // Local environmental influences -- the only part of the field the GPU fold can use.
    if (const env::EnvSystem* e = env::find_env(scene)) st.env = e->field();
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
                state.surface_up    = info.up;
                state.fog_color     = info.body->underwater_color;
                state.visibility    = info.body->underwater_visibility;
                state.absorption    = info.body->underwater_absorption;
                state.caustics      = info.body->caustics;
            }
        }
    }
    pipeline_->set_water_state(std::move(state));

    // A planet terrain gives the sky and fog a planet to be evaluated on (render::PlanetFrameState).
    std::optional<render::PlanetFrameState> planet;
    for (const world::TerrainComponent* terrain : scene.get_components<world::TerrainComponent>()) {
        if (!terrain->params.planet || !terrain->owner || !terrain->owner->get_transform()) continue;
        planet = render::PlanetFrameState{glm::vec3(terrain->owner->get_transform()->get_world_matrix()[3]),
                                          terrain->params.layout().radius()};
        break;
    }
    pipeline_->set_planet(planet);
}
} // namespace core
} // namespace toy
