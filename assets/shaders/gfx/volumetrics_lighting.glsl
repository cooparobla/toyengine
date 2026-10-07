#ifndef GFX_VOLUMETRICS_LIGHTING_GLSL
#define GFX_VOLUMETRICS_LIGHTING_GLSL

// gfx/volumetrics_lighting.glsl -- in-scatter lighting for the local volumes, shared by the
// two volumetrics modes: the per-pixel raymarch (volumetrics_march.frag) and the froxel grid
// (volumetrics_froxel_inject.frag). One body, so both modes light a volume identically.
//
// Requires, declared before inclusion: u_vol (gfx/volumetrics_ubo.glsl), the set-2 shadow
// sampler dir_shadow_map (sampler2DShadow), gfx/local_shadow.glsl (with GFX_LOCAL_SHADOWS set to
// u_vol.local_shadows and local_shadow_atlas bound), gfx_spot_cone (gfx/spot_light.glsl) and
// gfx_fog_hg (gfx/fog.glsl).
//
// Every shadow lookup takes the point's LIGHT-SPACE CLIP position from the caller rather than
// the world point: the march carries those along its ray as origin + t * direction (linear in
// t, so no per-step matrix product), while a froxel projects its single point directly.

// Clip-space position (pre-divide) -> [0,1] shadow coords, or false when the point
// lands outside the map. Out of bounds / behind the map counts as fully lit: a sample
// outside the fitted shadow frustum carries no occlusion information, and darkening it
// would draw the frustum's edges into the fog as a visible box.
bool vol_shadow_proj(vec4 lsp, float inset, out vec3 pc) {
    if (lsp.w <= 0.0) return false;
    pc = lsp.xyz / lsp.w;
    pc.xy = pc.xy * 0.5 + 0.5;
    return pc.z >= 0.0 && pc.z <= 1.0 &&
           all(greaterThanEqual(pc.xy, vec2(inset))) &&
           all(lessThanEqual(pc.xy, vec2(1.0 - inset)));
}

// Sun visibility from directional-shadow cascade `c`, given the point's clip position in
// that cascade (`lsp`). Returns false when the point is outside c's inset tile -- the caller
// then tries the next cascade (first containing cascade wins, as gfx_csm_select() does;
// cascade 0 is the sharpest). No transition dither: a hard cascade switch is invisible in
// fog, where a shadow tap only modulates in-scatter.
bool vol_sun_cascade_vis(int c, vec4 lsp, out float vis) {
    vec3 pc;
    if (!vol_shadow_proj(lsp, u_vol.dir_cascade_info.z, pc)) return false;
    float grid_x     = max(u_vol.dir_cascade_info.y, 1.0);
    vec2  tile_scale = vec2(1.0 / grid_x, 1.0 / ceil(max(u_vol.dir_cascade_info.x, 1.0) / grid_x));
    vec2  origin     = vec2(mod(float(c), grid_x), floor(float(c) / grid_x)) * tile_scale;
    vec2  uv         = origin + clamp(pc.xy, vec2(0.0), vec2(1.0)) * tile_scale;
    vis = 1.0 - texture(dir_shadow_map, vec3(uv, pc.z - u_vol.shadow_params.z)) * u_vol.shadow_params.y;
    return true;
}

// In-scatter from scatter light `li` at point p, seen along view_dir. A light with a shadow slot
// (params.w, 1-based) is shadowed with one tap of the local-light atlas -- point lights included.
vec3 vol_scatter_light(int li, vec3 p, vec3 view_dir, float light_strength) {
    vec3  to_light = u_vol.scatter_lights[li].position_range.xyz - p;
    float dist     = length(to_light);
    float range    = u_vol.scatter_lights[li].position_range.w;
    if (dist > range || dist < 1e-4) return vec3(0.0);
    vec3 L = to_light / dist;

    float cone = 1.0;
    if (u_vol.scatter_lights[li].params.z > 0.5) {
        cone = gfx_spot_cone(L, u_vol.scatter_lights[li].direction_cone.xyz,
                             u_vol.scatter_lights[li].direction_cone.w,
                             u_vol.scatter_lights[li].params.y);
        if (cone <= 0.0) return vec3(0.0);
    }

    // Same distance curve as the lighting pass's point/spot loops (pixel_lighting.frag),
    // so a light's glow in a volume matches its glow on the surfaces around it.
    float sharpness = max(u_vol.scatter_lights[li].params.x, 0.1);
    float factor    = clamp(dist / range, 0.0, 1.0);
    float falloff   = clamp(1.0 - pow(factor, sharpness), 0.0, 1.0);
    falloff *= falloff;
    float attenuation = falloff / (12.566370614 * (factor * factor + 1.0)); // 4*pi
    vec3 radiance = u_vol.scatter_lights[li].color_intensity.rgb
                  * (u_vol.scatter_lights[li].color_intensity.w * 0.08)
                  * attenuation * cone;

    float vis = 1.0;
    int slot = int(u_vol.scatter_lights[li].params.w + 0.5) - 1;
    if (slot >= 0) {
        vis = 1.0 - gfx_local_shadow_tap(slot, p) * u_vol.shadow_params.y;
    }

    // Same HG phase as the sun term: the anisotropy is a property of the medium's phase
    // function, whichever light the energy came from.
    float phase_l = gfx_fog_hg(dot(view_dir, L), u_vol.march_params.w);
    return radiance * (phase_l * light_strength * vis);
}

#endif // GFX_VOLUMETRICS_LIGHTING_GLSL
