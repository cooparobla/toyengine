#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(binding = 0) uniform sampler2D u_src;

layout(push_constant) uniform PushConstants {
    ivec2 src_size;
    int   is_first_pass;
} u_push;

void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);

    if (u_push.is_first_pass != 0) {
        // Mip 0 is a straight copy of the lit HDR scene colour (or, on frames with history, the
        // previous frame's final HDR copied in by SceneColorMipPass::copy_level0_from() and this
        // pass skipped). gl_FragCoord.xy is a 1:1 texel index here regardless of the source having
        // been rendered with a negative-height viewport, so this is an exact identity copy and the
        // mip chain shares ssr.frag's UV convention for free.
        out_color = vec4(texelFetch(u_src, min(c, u_push.src_size - 1), 0).rgb, 1.0);
        return;
    }

    // 2x2 box average of the previous mip. Deliberately NOT hiz_downsample.frag's conservative
    // 3x3 tap: dropping the last row/column at an odd source size costs a sliver of blur
    // accuracy here, whereas dropping it from a MIN reduction would break the Hi-Z pyramid's
    // conservativeness outright. Alpha is not a coverage weight: the chain holds an opaque
    // image (a final HDR frame's alpha is whatever its last blend left there), so it is
    // ignored and written as 1.
    ivec2 b = c * 2;
    vec3 s0 = texelFetch(u_src, min(b + ivec2(0, 0), u_push.src_size - 1), 0).rgb;
    vec3 s1 = texelFetch(u_src, min(b + ivec2(1, 0), u_push.src_size - 1), 0).rgb;
    vec3 s2 = texelFetch(u_src, min(b + ivec2(0, 1), u_push.src_size - 1), 0).rgb;
    vec3 s3 = texelFetch(u_src, min(b + ivec2(1, 1), u_push.src_size - 1), 0).rgb;
    out_color = vec4(0.25 * (s0 + s1 + s2 + s3), 1.0);
}
