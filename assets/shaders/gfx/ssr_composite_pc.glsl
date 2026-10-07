#ifndef GFX_SSR_COMPOSITE_PC_GLSL
#define GFX_SSR_COMPOSITE_PC_GLSL

// gfx/ssr_composite_pc.glsl -- push-constant block shared by every
// ssr_composite.frag, matching SsrPass::CompositePushConstants (ssr_pass.h)
// field-for-field. A consumer that doesn't use a field (blendy has no SSGI
// bounce term) simply never reads it; the C++ side always pushes the full
// union struct regardless of which shader is bound.
layout(push_constant) uniform CompositePushConstants {
    vec2  ssr_resolution;      // offset 0  -- resolution of u_ssr_map (trace res under half-res)
    vec2  screen_resolution;   // offset 8  -- always full screen res
    int   half_res;            // offset 16
    int   max_color_mip;       // offset 20 -- top mip of the prefiltered scene-colour chain
    float sky_intensity;       // offset 24 -- MUST match the lighting pass's sky_intensity
    float ssgi_intensity;      // offset 28 -- 0 disables the diffuse-bounce term
    float ssgi_distance;       // offset 32 -- world-space offset along N for the FALLBACK bounce tap
    float ssgi_traced;         // offset 36 -- > 0.5: read u_ssgi_map (the traced+resolved bounce)
                               //              instead of the normal-offset mip tap
    // std430 auto-aligns vec4 to the next 16-byte boundary, so these start at offset 48
    // (matching SsrPass::CompositePushConstants' explicit _pad1/_pad2 on the C++ side).
    vec4  sky_zenith;          // offset 48 -- MUST match the lighting pass's sky colours
    vec4  sky_horizon;         // offset 64
    vec4  sky_ground;          // offset 80
} pc;

#endif // GFX_SSR_COMPOSITE_PC_GLSL
