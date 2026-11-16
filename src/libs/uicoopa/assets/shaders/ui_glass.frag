#version 450

// UI "glass" fragment shader -- a screen-locked frosted pane for Context::glass()/
// glass_panel(). Samples the host's blurred-backdrop image (set via UiPass::set_backdrop_view(),
// bound here as an ordinary set-0 texture -- a glass quad's texture key IS the backdrop view,
// the same way a font atlas's view selects the "text" variant) at this fragment's position on
// the window, not at the quad's own uv: a glass pane does not scroll with its contents, which
// is what a backdrop filter is. v_uv is therefore unused -- DrawList::add_rounded_rect() writes
// (0.5, 0.5) on every vertex of a glass quad, so this variant is screen-locked by construction.
//
// gl_FragCoord is FRAMEBUFFER-relative, immune to a canvas's own origin/scale, so no extra
// plumbing is needed for a canvas placed inside part of the target (e.g. the editor's play
// viewport). The remap below mirrors UiPass::backdrop_remap()/backdrop_uv() byte-for-byte --
// those are the function to re-derive this from if the two ever need to change together, since
// no test can catch a C++/GLSL divergence directly.
//
// The backdrop is the frame BEFORE any UI was drawn, captured once per frame, in sRGB-encoded
// display space (the same space ui_composite.frag already composites in) -- so (a) glass
// stacked over glass does not see the lower pane, and (b) a world-space canvas composited by
// ui_composite_pass_ is covered, not blurred, where a glass pane overlaps it. Both are the
// correct trade for one blur per frame instead of one per pane.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_backdrop;

layout(push_constant) uniform Push {
    vec2 scale;
    vec2 offset;
    vec2 backdrop_origin;
    vec2 backdrop_inv_size;
} pc;

void main() {
    vec2 uv = clamp((gl_FragCoord.xy - pc.backdrop_origin) * pc.backdrop_inv_size, vec2(0.0), vec2(1.0));
    // Multiplied by v_color exactly as ui_quad.frag's texture(tex, v_uv) * v_color is, so alpha
    // stays a real "how frosted" knob under the existing AlphaOver blend: alpha = 1 is the pure
    // blurred backdrop, alpha < 1 blends it with whatever is already in the overlay target.
    out_color = vec4(texture(u_backdrop, uv).rgb * v_color.rgb, v_color.a);
}
