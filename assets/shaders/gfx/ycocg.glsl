#ifndef GFX_YCOCG_GLSL
#define GFX_YCOCG_GLSL

// gfx/ycocg.glsl -- RGB <-> YCoCg, for the temporal resolves that take their neighbourhood
// statistics in that space.
//
// YCoCg's luma/chroma separation makes a variance box tighter than an RGB box around the same
// samples, so a clip against it rejects ghosts sooner without eating real detail. Shared by
// taa.frag and ssr_resolve.frag rather than duplicated, the same rule every other gfx/*.glsl
// body follows. Declares no uniforms or samplers: every input is a parameter.

vec3 gfx_rgb_to_ycocg(vec3 c) {
    return vec3( 0.25 * c.r + 0.50 * c.g + 0.25 * c.b,
                 0.50 * c.r               - 0.50 * c.b,
                -0.25 * c.r + 0.50 * c.g - 0.25 * c.b);
}

vec3 gfx_ycocg_to_rgb(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

#endif // GFX_YCOCG_GLSL
