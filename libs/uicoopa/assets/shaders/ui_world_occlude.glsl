// Shared occlusion test for UiWorldPass's two fragment variants -- see ui_world_quad.frag
// and ui_world_text.frag. A .glsl body rather than duplicated source so the two can never
// drift, following gfxcoopa's own gfx/*.glsl convention.
//
// Why a shader-side discard instead of a hardware depth test: a world canvas composites
// into whatever colour target the host already has open, and that target generally has no
// *scene* depth attached (toyengine's is a post-tonemap buffer whose own depth attachment is
// cleared and never written). So the real scene depth arrives as a sampled texture instead,
// supplied by the host at set 1 via TexturedQuad2DDesc::extra.
//
// The set is declared unconditionally and the test branches on a push constant, rather than
// splitting into occluded/non-occluded shader variants. ExtraSets exists to make "a pipeline
// genuinely built without the set" possible, but it lives on TexturedQuad2DDesc, not per
// variant -- so variants could not differ in it anyway, and a second pass instance would
// duplicate the descriptor cache and both per-frame-in-flight geometry buffers for nothing.
// The branch is uniform across a draw, so it costs nothing measurable.
//
// Hosts with no scene depth to offer bind a 1x1 texel of 1.0, which never occludes.

layout(set = 1, binding = 0) uniform sampler2D u_scene_depth;

// Returns true if this fragment is behind opaque scene geometry and should be discarded.
bool ui_world_occluded(float occlude, vec2 inv_target_size, float depth_bias) {
    if (occlude <= 0.5) return false;

    // gl_FragCoord is framebuffer-space with a TOP-LEFT origin regardless of the viewport's
    // negative height (the sign affects the NDC->framebuffer mapping, not gl_FragCoord's own
    // origin), and .xy is already texel-centred. u_scene_depth was rasterized through the
    // same negative-height viewport at the same extent, so its texel (x, y) IS this
    // framebuffer pixel -- no Y flip. (Cross-check: gfxcoopa's stylize.frag samples
    // this same depth image via fullscreen.vert's v_uv, which maps uv (0,0) to the
    // framebuffer's top-left the same way.)
    float scene_z = texture(u_scene_depth, gl_FragCoord.xy * inv_target_size).r;

    // Not reversed-Z: this engine uses CompareOp::Less with GLM_FORCE_DEPTH_ZERO_TO_ONE, so
    // gl_FragCoord.z is in [0, 1] with larger meaning farther, and a depth buffer cleared to
    // 1.0 (open sky) never occludes anything. depth_bias gives a canvas sitting nearly
    // coplanar with a surface enough slack not to z-fight against it.
    return gl_FragCoord.z > scene_z + depth_bias;
}
