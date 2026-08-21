#version 450

// Temporary Phase-2 milestone shader: an 8x8-texel checkerboard blended with
// a UV gradient, drawn into the low-resolution offscreen target so the
// integer upscale + nearest filtering can be visually verified before any
// real geometry exists. Superseded by pixel_lighting.frag once the G-buffer
// and lighting pass land (Phase 3/4) -- left in place afterward as a
// standalone diagnostic for pixel-grid alignment.

layout(location = 0) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

void main() {
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float check = mod(float((texel.x / 8) + (texel.y / 8)), 2.0);
    vec3 a = vec3(in_uv, 0.5);
    vec3 b = vec3(1.0 - in_uv.x, 1.0 - in_uv.y, 0.2);
    out_color = vec4(mix(a, b, check), 1.0);
}
