#ifndef GFX_VOLUMETRICS_UBO_GLSL
#define GFX_VOLUMETRICS_UBO_GLSL

// gfx/volumetrics_ubo.glsl -- the VolumetricsUBO block (set 1, binding 0), shared by
// volumetrics_march.frag and volumetrics_composite.frag. Must match VolumetricsUBO in
// engine/data/volumetrics_data.h field for field.

#include <gfx/local_shadow_types.glsl>

struct Volume {
    mat4 inv_world;
    vec4 extent_shape;     // xyz = half-extent (sphere uses .x), w = 0 Box / 1 Sphere
    vec4 direction_speed;  // xyz = normalized advection direction, w = speed
    vec4 field_params;     // x = noise scale, y = streak, z = coverage, w = fbm gain
    vec4 shape_params;     // x = density, y = height base, z = height falloff, w = octaves
    vec4 flow_params;      // x = meander amp, y = meander freq, z = sharpness, w = gate freq
    vec4 color_occlusion;  // rgb = scatter colour, w = occlusion scale
    vec4 mode_params;      // x = kind, y = sun amount, z = edge softness
};

// A point or spot light that in-scatters into the march -- a trimmed copy of the
// lighting pass's per-light data (see ScatterLightGPU, volumetrics_data.h).
struct ScatterLight {
    vec4 position_range;   // xyz = world position, w = range
    vec4 color_intensity;  // rgb = colour, w = intensity
    vec4 direction_cone;   // xyz = spot direction, w = cos(outer); ignored for points
    vec4 params;           // x = falloff sharpness, y = cos(inner), z = 1 spot / 0 point,
                           // w = shadow slot in local_shadows, 1-based (0 = unshadowed)
};

layout(set = 1, binding = 0) uniform VolumetricsUBO {
    mat4 inv_view_proj;
    vec4 camera_pos;     // xyz = world camera position, w = debug view flag
    vec4 sun_direction;  // xyz = direction the light travels
    vec4 sun_color;      // rgb = sun colour * intensity
    vec4 march_params;   // x = step count, y = max distance, z = max opacity, w = sun anisotropy
    vec4 time_params;    // x = elapsed time, y = delta time, z = frame index
    vec4 counts;         // x = active volume count, y = scatter light count,
                         // z = light-scatter strength (0 skips the light loop)
    Volume volumes[8];
    mat4 dir_light_space_matrix;   // world -> cascade 0's shadow clip; the sun term reads
                                   // dir_cascade_matrix below instead, but the field holds
                                   // this block's place in the std140 layout
    mat4 spot_light_space_matrix;  // world -> spot shadow clip
    vec4 shadow_params;  // x = shadow the sun term (0/1), y = shadow strength,
                         // z = dir depth bias, w = spot depth bias
    ScatterLight scatter_lights[4];
    // Directional shadow cascades -- see VolumetricsUBO (volumetrics_data.h). The
    // directional map is a tile atlas, so dir_light_space_matrix above only reaches
    // cascade 0; a shaft marching the full distance needs the whole set.
    mat4 dir_cascade_matrix[4];
    vec4 dir_cascade_info; // x = cascade count, y = tiles per atlas row, z = selection
                           // inset in tile uv, w = dither band (unused here)
    // Froxel mode only (FroxelVolumetricsPass).
    mat4 prev_view_proj;   // last frame's unjittered view-projection
    vec4 froxel_grid;      // x = W, y = H, z = D (slices), w = slices per atlas row
    vec4 froxel_params;    // x = near, y = far, z = history weight, w = history valid
    vec4 prev_camera_pos;  // xyz = last frame's camera position
    vec4 froxel_params2;   // x = samples per froxel on a history miss,
                           // y = composite lookup jitter (froxels / slices; 0 = off)
    // Point/spot shadows -- LightUBO::local_shadows' copy (gfx/local_shadow.glsl).
    GfxLocalShadowBlock local_shadows;
} u_vol;

#endif // GFX_VOLUMETRICS_UBO_GLSL
