#include <toyengine/render/toy_render_config.h>

namespace toy {
namespace render {

DebugView parse_debug_view(const std::string& value) {
    if (value == "off")             return DebugView::Off;
    if (value == "albedo")          return DebugView::Albedo;
    if (value == "normals")         return DebugView::Normals;
    if (value == "roughness")       return DebugView::Roughness;
    if (value == "metallic")        return DebugView::Metallic;
    if (value == "emissive")        return DebugView::Emissive;
    if (value == "material_ao")     return DebugView::MaterialAo;
    if (value == "world_pos")       return DebugView::WorldPos;
    if (value == "depth")           return DebugView::Depth;
    if (value == "direct")          return DebugView::Direct;
    if (value == "indirect")        return DebugView::Indirect;
    if (value == "shadows")         return DebugView::Shadows;
    if (value == "contact_shadows") return DebugView::ContactShadows;
    if (value == "ssao")            return DebugView::Ssao;
    if (value == "ssr")             return DebugView::Ssr;
    if (value == "ssr_confidence")  return DebugView::SsrConfidence;
    if (value == "ssgi")            return DebugView::Ssgi;
    if (value == "dof")             return DebugView::Dof;
    if (value == "volumetrics")     return DebugView::Volumetrics;
    if (value == "lines")           return DebugView::Lines;
    if (value == "solid")           return DebugView::Solid;
    if (value == "wireframe")       return DebugView::Wireframe;
    if (value == "material_preview") return DebugView::MaterialPreview;
    if (value == "velocity")        return DebugView::Velocity;
    std::cerr << "[toy::render] Unknown debug_view '" << value << "', expected one of: "
                 "off | albedo | normals | roughness | metallic | emissive | material_ao | "
                 "world_pos | depth | velocity | direct | indirect | shadows | contact_shadows | ssao | "
                 "ssr | ssr_confidence | ssgi | dof | volumetrics | lines | solid | wireframe | material_preview. Using 'off'.\n";
    return DebugView::Off;
}

bool debug_view_is_channel(DebugView view) {
    switch (view) {
        case DebugView::Off:
        case DebugView::Dof:
        case DebugView::Volumetrics:
        case DebugView::Lines:
            return false;
        default:
            return true;
    }
}

bool debug_view_is_editor_shading(DebugView view) {
    return view == DebugView::Solid || view == DebugView::Wireframe || view == DebugView::MaterialPreview;
}

void ToyRenderConfig::apply_quality_presets() {
    // shadow_pcss_taps/contact_shadow_steps only cost anything when their own feature
    // toggle is on, so scaling them here is free for a config that leaves both off.
    switch (shadow_quality) {
        // shadow_map_resolution is PER CASCADE (see its doc): the directional atlas is up
        // to 2x this on each axis, so the VRAM column at the default 4 cascades is
        // 4/16/64/144 MB. Medium matches the VRAM of one 2048 map, and high spends 4x that
        // so every cascade beats a single 2048 map's world-per-texel at every distance.
        case RenderQuality::Low:    shadow_map_resolution = 512;  cube_shadow_resolution = 256;  spot_shadow_resolution = 512;  shadow_pcf_samples = 8;  shadow_pcss_taps = 4;  contact_shadow_steps = 4;  break;
        case RenderQuality::Medium: shadow_map_resolution = 1024; cube_shadow_resolution = 512;  spot_shadow_resolution = 1024; shadow_pcf_samples = 16; shadow_pcss_taps = 6;  contact_shadow_steps = 6;  break;
        case RenderQuality::High:   shadow_map_resolution = 2048; cube_shadow_resolution = 512;  spot_shadow_resolution = 1024; shadow_pcf_samples = 24; shadow_pcss_taps = 8;  contact_shadow_steps = 8;  break;
        case RenderQuality::Ultra:  shadow_map_resolution = 3072; cube_shadow_resolution = 1024; spot_shadow_resolution = 2048; shadow_pcf_samples = 32; shadow_pcss_taps = 16; contact_shadow_steps = 16; break;
    }
    // The local-light atlas and budgets per tier, sized so the budget packs: a point light is
    // a 3x2 block of cube tiles, a spot one spot tile (see pack_shadow_atlas()).
    switch (shadow_quality) {
        case RenderQuality::Low:    local_shadow_atlas_resolution = 1024; max_shadowed_point_lights = 1; max_shadowed_spot_lights = 1; break;
        case RenderQuality::Medium: local_shadow_atlas_resolution = 2048; max_shadowed_point_lights = 1; max_shadowed_spot_lights = 2; break;
        case RenderQuality::High:   local_shadow_atlas_resolution = 4096; max_shadowed_point_lights = 4; max_shadowed_spot_lights = 4; break;
        case RenderQuality::Ultra:  local_shadow_atlas_resolution = 6144; max_shadowed_point_lights = 4; max_shadowed_spot_lights = 3; break;
    }
    switch (ssao_quality) {
        case RenderQuality::Low:    ssao_slices = 1; ssao_steps = 6;  ssao_max_radius_px = 32.0f; ssao_temporal_frames = 4;  break;
        case RenderQuality::Medium: ssao_slices = 2; ssao_steps = 8;  ssao_max_radius_px = 40.0f; ssao_temporal_frames = 8;  break;
        case RenderQuality::High:   ssao_slices = 2; ssao_steps = 16; ssao_max_radius_px = 80.0f; ssao_temporal_frames = 8;  break;
        case RenderQuality::Ultra:  ssao_slices = 3; ssao_steps = 24; ssao_max_radius_px = 96.0f; ssao_temporal_frames = 12; break;
    }
    switch (ssr_quality) {
        case RenderQuality::Low:    ssr_max_iterations = 24;  ssr_rays_per_pixel = 1; break;
        case RenderQuality::Medium: ssr_max_iterations = 48;  ssr_rays_per_pixel = 1; break;
        case RenderQuality::High:   ssr_max_iterations = 64;  ssr_rays_per_pixel = 1; break;
        case RenderQuality::Ultra:  ssr_max_iterations = 128; ssr_rays_per_pixel = 2; break;
    }
    // Lower across the board than ssr_quality's rows, and deliberately: the bounce ray is
    // short and lands in a coarse cone mip, so it converges in far fewer steps. This is
    // the single biggest cost dial the traced bounce has -- it is three full-resolution
    // passes (trace, resolve, denoise) when ssgi_traced is on.
    switch (ssgi_quality) {
        case RenderQuality::Low:    ssgi_max_iterations = 12; ssgi_resolution_scale = 2; break;
        case RenderQuality::Medium: ssgi_max_iterations = 20; ssgi_resolution_scale = 2; break;
        case RenderQuality::High:   ssgi_max_iterations = 32; ssgi_resolution_scale = 1; break;
        case RenderQuality::Ultra:  ssgi_max_iterations = 48; ssgi_resolution_scale = 1; break;
    }
    switch (dof_quality) {
        case RenderQuality::Low:    dof_sample_count = 16; break;
        case RenderQuality::Medium: dof_sample_count = 32; break;
        case RenderQuality::High:   dof_sample_count = 48; break;
        case RenderQuality::Ultra:  dof_sample_count = 48; break; // dof.frag clamps taps to [8, 48]
    }
    // The two multiply: every scatter light is evaluated at every step. Low drops the
    // light loop entirely and keeps only the sun-shaft term, which is one shadow tap.
    switch (volumetrics_quality) {
        case RenderQuality::Low:    volumetrics_step_count = 24; volumetrics_max_scatter_lights = 0; break;
        case RenderQuality::Medium: volumetrics_step_count = 32; volumetrics_max_scatter_lights = 2; break;
        case RenderQuality::High:   volumetrics_step_count = 48; volumetrics_max_scatter_lights = 4; break;
        case RenderQuality::Ultra:  volumetrics_step_count = 96; volumetrics_max_scatter_lights = 4; break; // 4 == MAX_SCATTER_LIGHTS
    }
    switch (sdf_quality) {
        case RenderQuality::Low:    sdf_max_steps = 32;  sdf_shadow_max_steps = 16; break;
        case RenderQuality::Medium: sdf_max_steps = 48;  sdf_shadow_max_steps = 24; break;
        case RenderQuality::High:   sdf_max_steps = 64;  sdf_shadow_max_steps = 32; break;
        case RenderQuality::Ultra:  sdf_max_steps = 128; sdf_shadow_max_steps = 64; break;
    }
}

} // namespace render
} // namespace toy
