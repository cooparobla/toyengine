#version 450

// XrayPass's composite stage -- see toyengine/render/passes/xray_pass.h.
//
// Draws the x-ray look over the finished frame from the coverage mask xray_mask.frag filled:
//
//   R  subject hidden        G  occluder permits        B  subject coverage        A  closeness
//
// A guest inside post_target_'s already-open bracket, AFTER the stylize pass, exactly like
// DebugLinePass. That placement is deliberate: the colour an author picks lands as picked,
// instead of being pushed through ACES, the grading LUT and the palette quantizer, and the
// silhouette never feeds bloom or the stylize outline's edge detector. It is still upstream
// of AA, so its own edges get antialiased.
//
// Everything is gated on `permit` (G), so the silhouette appears only where a surface that
// opted into being seen through is the thing covering the subject.

layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D u_mask;

layout(push_constant) uniform XrayCompositePC {
    vec4  color;        // rgb = silhouette/outline colour, a = how far it blends over the scene
    vec2  inv_size;     // 1 / mask size, in texels
    float thickness;    // outline half-width in texels; <= 0 draws no outline
    float depth_fade;   // 0..1: how much the deeper-hidden parts of the subject dim
    float fill;         // 1 = fill the hidden area (silhouette), 0 = outline only
    float outline;      // 1 = draw the outline, 0 = none
    float pad0;
    float pad1;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    const vec4  m      = texture(u_mask, in_uv);
    const float permit = m.g;
    if (permit <= 0.0) discard;   // nothing here opted into being seen through

    // The outline is the boundary of the subject's COVERAGE (B) -- see xray_mask.frag on why
    // not R -- found by a ring tap rather than a full box: a silhouette edge is what we want,
    // and a ring of four finds one at any thickness for four taps instead of thickness^2.
    //
    // Still gated on `permit` above, so a fully visible subject draws nothing: at its own
    // pixels the visible surface is the subject itself, not an occluder, so G is 0 there.
    float edge = 0.0;
    if (pc.outline > 0.5 && pc.thickness > 0.0) {
        const vec2 t = pc.inv_size * pc.thickness;
        // Inside the shape and next to something outside it -> on the boundary.
        const float n = texture(u_mask, in_uv + vec2(0.0,  t.y)).b;
        const float s = texture(u_mask, in_uv + vec2(0.0, -t.y)).b;
        const float e = texture(u_mask, in_uv + vec2( t.x, 0.0)).b;
        const float w = texture(u_mask, in_uv + vec2(-t.x, 0.0)).b;
        const float outside = max(max(1.0 - n, 1.0 - s), max(1.0 - e, 1.0 - w));
        edge = m.b * outside;
    }

    // The fill is the HIDDEN area only (R), not the whole coverage -- the visible part of the
    // subject is already drawn properly and must not be painted over.
    const float fill = pc.fill > 0.5 ? m.r : 0.0;

    // Dim with distance behind the covering surface. A (closeness) is 1 right behind it, so
    // depth_fade 0 leaves this at 1 and the whole silhouette reads at one strength.
    const float strength = mix(1.0, m.a, clamp(pc.depth_fade, 0.0, 1.0));

    const float coverage = clamp(max(fill * strength, edge), 0.0, 1.0) * permit;
    if (coverage <= 0.0) discard;
    out_color = vec4(pc.color.rgb, coverage * pc.color.a);
}
