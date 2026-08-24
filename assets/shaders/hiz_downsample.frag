#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out float out_depth;

layout(binding = 0) uniform sampler2D u_input_depth;

layout(push_constant) uniform PushConstants {
    ivec2 src_size;
    int is_first_pass;
} u_push;

void main() {
    ivec2 frag_coord = ivec2(gl_FragCoord.xy);
    if (u_push.is_first_pass != 0) {
        out_depth = texelFetch(u_input_depth, frag_coord, 0).r;
    } else {
        // Conservative 3x3 tap (not 2x2): at an odd source dimension, a plain 2x2 footprint
        // never reaches the last row/column (e.g. 135 -> 67 only ever samples rows 0..133,
        // dropping row 134 from every reduction), silently making the pyramid non-conservative
        // from that mip upward. The extra row/column only ever shrinks the min further, so this
        // is safe (still correct, just marginally tighter) at even sizes too.
        ivec2 base_coord = frag_coord * 2;
        float min_depth = 1.0;
        for (int dy = 0; dy < 3; ++dy) {
            for (int dx = 0; dx < 3; ++dx) {
                ivec2 c = min(base_coord + ivec2(dx, dy), u_push.src_size - 1);
                min_depth = min(min_depth, texelFetch(u_input_depth, c, 0).r);
            }
        }
        out_depth = min_depth;
    }
}
