#ifndef TOY_LIGHT_UBO_BODY_GLSL
#define TOY_LIGHT_UBO_BODY_GLSL

// light_ubo_body.glsl -- the `lights` uniform block, at set 1 binding 0.
//
// A "body" file in the toy_shadow_body.glsl / contact_shadow_body.glsl sense: every shader
// that shades from toyengine's light set includes this instead of restating the block. The
// layout must match gfxcoopa's LightUBO (light_data.h) field for field, and the consequence of
// two copies drifting is silent garbage -- a shader reading spot_shadow_params out of what the
// CPU wrote as sky_ground -- so there is exactly one copy.
//
// REQUIRED BEFORE INCLUDE:
//   - #include <gfx/spot_light.glsl>, for the SpotLight struct.
//
// The set index is fixed at 1 here rather than parameterised: every consumer binds toyengine's
// light set at 1 (toy_lighting.frag, debug_view.frag, contact_shadow.frag,
// sdf_forward.frag, gfx/surface/transparent_fs.glsl), and a shader
// that needed it elsewhere would be diverging from the descriptor contract they all share.

#include <gfx/local_shadow_types.glsl>
#include <gfx/fog_types.glsl>

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
    mat4 spot_light_space_matrix; // unread: spot shadows come from local_shadows; kept for layout
    vec4 spot_shadow_params;      // unread, zeroed on the CPU; see local_shadows
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

    // The physical sky (render sky_model: physical) -- see light_data.h. All zero (gradient sky)
    // unless the pipeline's SkyAtmospherePass is drawing it.
    vec4 sky_sun;      // xyz = unit direction TO the sun, w = cos(disc angular radius)
    vec4 sky_moon;     // xyz = unit direction TO the moon, w = cos(disc angular radius)
    vec4 sky_params;   // x = 1 physical sky, y = cloud march scale (0 = no clouds composited), z = star visibility, w = sky illuminance
    vec4 sky_extra;    // x = sun disc radiance, y = moon disc radiance, z = night fill, w = time (s)
                       //
                       // sky_extra.z has two readings, and they can never both be live in one
                       // frame, so no caller has to disambiguate:
                       //  - physical sky: an airglow floor, scaled by 1e-3 where sky_physical()
                       //    uses it, so a moonless night sky is not pure black.
                       //  - PLANET (sky_model: space): the moon's radiance, in the same units as
                       //    dir_color -- what lights the hemisphere facing away from the sun, for
                       //    the ground (toy_lighting.frag) and the cloud deck (sky_clouds.frag)
                       //    alike. Both of those gate on their own planet slot being set, and
                       //    sky_physical() never runs under a space sky. 0 = a black night side,
                       //    which is the default and what every scene without a moon gets.

    // Global exponential height fog -- see gfx/fog.glsl. Read by fog.frag (the opaque scene)
    // and by every forward shader, which fogs its own fragment at its own distance.
    GfxFogBlock fog;

    // The cloud layer's shadow map (render cloud_shadows) -- see cloud_shadow.glsl.
    vec4 cloud_shadow;        // xy = the map's corner (world), z = 1 / its side, w = strength (0 = off)
    vec4 cloud_shadow_layer;  // x = the layer's base height (world z), y = its thickness, z = low-light fade

    // Per-material toon shading, scene-wide tuning -- see gfx/toon.glsl, which these two are
    // handed to as-is by gfx_toon_unpack(). The PER-MATERIAL half of the settings rides the
    // G-buffer's emissive alpha instead; these are only the continuous knobs that would not
    // survive that packing. toon_params0.w (master strength) = 0 shades every toon material in
    // the frame as plain PBR.
    vec4 toon_params0;  // x = specular step threshold (share of the GGX lobe's own peak),
                        // y = that step's edge width, z = rim falloff exponent, w = master strength
    vec4 toon_params1;  // rgb = shadow tint for the darkest bands (white = none),
                        // w = 1 bands point/spot distance falloff too

    // Extended directional shadow levels -- the FULL set (up to 8), appended per
    // light_data.h's append-only rule. calc_dir_shadow() reads these; dir_cascade_* above
    // keeps the first four levels' copy for consumers on the shorter std140 prefix.
    // Per-level scalars pack four to a vec4: level L is [L >> 2][L & 3] (toy_level_scalar()).
    mat4 dir_level_matrix[8];
    vec4 dir_level_pcf_texels[2];   // per-level PCF radius in atlas texels (0 = hard)
    vec4 dir_level_normal_bias[2];  // per-level normal offset, world units
    vec4 dir_level_pcss_scale[2];   // per-level PCSS texels per unit [0,1] depth gap
    vec4 dir_level_depth_bias[2];   // per-level [0,1] light depth of one texel
    vec4 dir_level_info;            // x=count, y=tiles per atlas row, z=selection inset
                                    // (tile uv), w=transition band (tile uv)
    vec4 dir_level_age[2];          // per-level frames since its tile was re-rendered
                                    // (debug view only; shading ignores it)

    // Additional sky bodies -- appended per light_data.h's append-only rule. Extra sun and moon
    // discs the sky draws beyond the single sky_sun / sky_moon pair; all zero (counts == 0)
    // keeps that pair authoritative.
    vec4 sky_body_counts;    // x = live sun count, y = live moon count
    vec4 sky_sun_dirs[4];    // xyz = unit dir TO sun i (sky_sun's frame), w = cos(disc radius)
    vec4 sky_sun_colors[4];  // rgb = tint, w = disc radiance
    vec4 sky_moon_dirs[8];   // xyz = unit dir TO moon i (sky_moon's frame), w = cos(disc radius)
    vec4 sky_moon_colors[8]; // rgb = tint, w = disc radiance before phase shading
    vec4 sky_moon_lit[8];    // xyz = unit dir from moon i TO the sun lighting it (same frame)
    vec4 sky_moon_rects[8];  // moon face in the sky-body atlas: xy = uv offset, zw = uv size;
                             // zw = 0 draws the procedural face
    vec4 sky_moonlight;      // xyz = WORLD-space unit dir TO the brightest moon (night ground
                             // light), w = 1 live / 0 anti-solar fallback
    vec4 sky_moonlight_tint; // rgb = the night-side moonlight's tint
    vec4 sky_backdrop;       // xyz = the star backdrop's galactic-band pole (sky_sun's frame),
                             // w = its variation seed; xyz = 0 keeps the built-in orientation
    vec4 sky_moon_atmo[8];   // moon i's limb shell: rgb = scatter tint, w = thickness as a
                             // fraction of the disc's radius; w = 0 draws no shell

    // Analytic occluders of the directional light -- appended per light_data.h's append-only
    // rule. See toy_dir_occlusion() below.
    vec4 dir_occluder_info;  // x = live count, y = the light's angular radius (rad), 0 = off
    vec4 dir_occluders[4];   // xyz = sphere centre, WORLD space; w = its radius

    // The world's frame -- appended per light_data.h's append-only rule. xyz = the centre the
    // world curves about, WORLD space (unlike sky_sun/sky_moon above, which are pre-rotated into
    // the camera's frame); w = its radius, and w <= 0 is a flat +Z-up world.
    vec4 world_frame;
} lights;

/**
 * How much of the directional light survives the occluder spheres at `world_pos`.
 *
 * The light is a disc of angular radius `dir_occluder_info.y`; each occluder is a disc of its
 * own, `asin(radius / distance)` across, centred on the direction to it. What is left is the
 * fraction of the first disc the others have not covered -- 1 in the open, 0 under a total
 * eclipse, and a smooth ramp through the penumbra as the discs slide past one another.
 *
 * Not a shadow map and deliberately so: a map is fitted to the camera's own few hundred metres,
 * and these spheres are thousands of units away. The cost is a handful of ALU per occluder.
 *
 * `to_light` points TO the light. Occluders behind the shaded point are skipped, as is any
 * sphere the point is inside or on -- a body must not eclipse itself, which is what its own
 * N.L already says.
 */
float toy_dir_occlusion(vec3 world_pos, vec3 to_light) {
    int n = int(lights.dir_occluder_info.x);
    float light_r = lights.dir_occluder_info.y;
    if (n <= 0 || light_r <= 0.0) return 1.0;

    float visible = 1.0;
    for (int i = 0; i < 4; ++i) {
        if (i >= n) break;
        vec3 to_centre = lights.dir_occluders[i].xyz - world_pos;
        float radius = lights.dir_occluders[i].w;
        float dist = length(to_centre);
        if (radius <= 0.0 || dist <= radius * 1.001) continue;   // inside it, or it is us
        vec3 dir = to_centre / dist;
        if (dot(dir, to_light) <= 0.0) continue;                 // behind us, not between

        float occ_r = asin(clamp(radius / dist, 0.0, 1.0));
        float sep = acos(clamp(dot(dir, to_light), -1.0, 1.0));
        // How far the two discs overlap, 0 at first contact .. 1 when the occluder's centre is
        // on the light's, and how much of the light such a disc can cover at most.
        float reach = light_r + occ_r;
        if (sep >= reach) continue;
        float overlap = clamp((reach - sep) / max(2.0 * min(light_r, occ_r), 1e-6), 0.0, 1.0);
        float cover = clamp((occ_r * occ_r) / (light_r * light_r), 0.0, 1.0);
        visible *= 1.0 - cover * smoothstep(0.0, 1.0, overlap);
    }
    return visible;
}

#endif // TOY_LIGHT_UBO_BODY_GLSL
