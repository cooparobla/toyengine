#ifndef GFX_SURFACE2D_QUAD_VS_GLSL
#define GFX_SURFACE2D_QUAD_VS_GLSL

// gfx/surface2d/quad_vs.glsl -- the one piece of math every 2D textured-quad vertex shader
// shares: an affine transform (scale + offset) into NDC, plus the Vulkan Y-flip.
//
// Not a backbone in the gfx/surface/*.glsl sense (no #include-and-inherit-a-main(), no
// hook/struct-mutation machinery) -- a push_constant block can only be declared ONCE per
// stage, and every consumer here needs to APPEND its own trailing fields (uicoopa: none
// beyond scale/offset; pixengine: a tint) after this shared prefix, so the block itself
// must live in each entry point, not in this shared file. What's shared is just the
// formula and the convention it encodes:
//
//   uicoopa (canvas space, origin bottom-left):  scale = inv_canvas_size * 2,  offset = -1
//   pixengine (camera space, world units):       scale = inv_half_extent,
//                                                 offset = -camera_pos * inv_half_extent
//
// Each consumer computes its own scale/offset at the C++ push-constant-fill call site (one
// multiply-add) and declares a push_constant block starting with `vec2 scale; vec2 offset;`
// before calling gfx_quad_2d_transform() -- see uicoopa's ui.vert and pixengine's sprite.vert
// for the two examples.
vec4 gfx_quad_2d_transform(vec2 pos, vec2 scale, vec2 offset) {
    vec2 ndc = pos * scale + offset;
    return vec4(ndc.x, -ndc.y, 0.0, 1.0);
}

#endif // GFX_SURFACE2D_QUAD_VS_GLSL
