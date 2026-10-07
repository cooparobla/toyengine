#ifndef TOY_LIGHT_UBO_BODY_GLSL
#define TOY_LIGHT_UBO_BODY_GLSL

// light_ubo_body.glsl -- the `lights` uniform block, at set 1 binding 0.
//
// A "body" file in the pixel_shadow_body.glsl / contact_shadow_body.glsl sense: every shader
// that shades from toyengine's light set includes this instead of restating the block. The
// layout must match gfxcoopa's LightUBO (light_data.h) field for field, and the consequence of
// two copies drifting is silent garbage -- a shader reading spot_shadow_params out of what the
// CPU wrote as sky_ground -- so there is exactly one copy.
//
// REQUIRED BEFORE INCLUDE:
//   - #include <gfx/spot_light.glsl>, for the SpotLight struct.
//
// The set index is fixed at 1 here rather than parameterised: every consumer binds toyengine's
// light set at 1 (pixel_lighting.frag, debug_view.frag, contact_shadow.frag,
// sdf_forward.frag, gfx/surface/transparent_fs.glsl), and a shader
// that needed it elsewhere would be diverging from the descriptor contract they all share.

#include <gfx/local_shadow_types.glsl>

struct PointLight {
    vec4 position_range;  // xyz = pos, w = range
    vec4 color_intensity; // xyz = color, w = intensity
    vec4 attenuation;     // x=const, y=lin, z=quad, w=shadow slot in local_shadows, 1-based (0 = none)
};

layout(set = 1, binding = 0) uniform LightUBO {
    vec4 dir_direction;
    vec4 dir_color;
    vec4 dir_shadow_extra; // x=shadow_intensity, y=unused (was the point PCF radius), z=pcf_samples, w=frame_offset
    mat4 dir_light_space_matrix;
    vec4 dir_shadow_params; // x=bias, y=pcf_radius_texels (0=hard), z=shadow_enabled, w=normal_bias

    uvec4 light_counts; // x=num_dir, y=num_point
    PointLight point_lights[16];

    // Configurable sky/ambient colour (see IndirectParams in render_features.h).
    // Trailing so no field above moves -- std140 only requires a matching prefix.
    vec4 sky_zenith;
    vec4 sky_horizon;
    vec4 sky_ground;

    // Spot Lights -- appended after sky_ground; see light_data.h's LightUBO doc on why
    // nothing above this line may move.
    mat4 spot_light_space_matrix; // LEGACY (gfxcoopa's own shaders): spot shadows now live in local_shadows
    vec4 spot_shadow_params;      // LEGACY: zeroed; see local_shadows
    SpotLight spot_lights[8];

    // Appended after spot_lights per light_data.h's append-only rule.
    vec4 pcss_params;    // x=enabled, y=penumbra texels per unit depth gap (cascade 0; the
                         // per-cascade values are in dir_cascade_pcss_scale), z=blocker search
                         // radius texels, w=search taps (see calc_dir_shadow)
    vec4 contact_params; // x=strength (0 disables), y=length m, z=thickness m,
                         // w=steps -- read by contact_shadow.frag's march
    vec4 contact_soft_params; // x=sun angular size tangent (soft_shadows on; 0 = hard),
                         // y=per-frame step rotation (0 when the contact resolve is off). z/w reserved.

    // Directional shadow cascades. The directional map is an ATLAS of up to 4 tiles, one per
    // cascade, each fit to its own slice of the camera's depth range -- see light_data.h's
    // cascade doc and gfx/shadow_sampling.glsl's gfx_csm_select(). dir_light_space_matrix /
    // dir_shadow_params.y / dir_shadow_params.w / pcss_params.y above are CASCADE 0's values.
    mat4 dir_cascade_matrix[4];
    vec4 dir_cascade_pcf_texels;   // per-cascade PCF radius in ATLAS texels (0 = hard)
    vec4 dir_cascade_normal_bias;  // per-cascade normal offset, world units
    vec4 dir_cascade_pcss_scale;   // per-cascade PCSS texels per unit [0,1] depth gap
    vec4 dir_cascade_info;         // x=count, y=tiles per atlas row, z=selection inset (tile uv),
                                   // w=dither transition band (tile uv)
    vec4 dir_shadow_receiver;      // x=1: receiver-plane depth bias on the directional PCF,
                                   // y=steepest receiver slope honoured (tan). z/w reserved.

    // Directional shadow bias and fade -- see light_data.h for each field's doc.
    vec4 dir_cascade_depth_bias;   // per-cascade [0,1] light depth of one shadow texel
    vec4 dir_shadow_bias_texels;   // x=constant texels, y=slope texels per tan, z=max tan
    vec4 dir_shadow_fade;          // xyz=camera pos, w=fully-faded distance (0 = off)
    vec4 dir_shadow_fade_params;   // x=fade start distance, y=last-cascade edge band (tile uv),
                                   // z=1 blend cascades / 0 dither

    // Point/spot shadows in the local-light atlas -- gfx/local_shadow.glsl samples these.
    GfxLocalShadowBlock local_shadows;
} lights;

#endif // TOY_LIGHT_UBO_BODY_GLSL
