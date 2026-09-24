#version 450

// Downsample shader for the SSAO march's prefiltered depth pyramid (see SsaoPass /
// ssao.frag). Unlike hiz_downsample.frag's min() reduction -- whose stored cell minimum
// the AO march would reconstruct at the sampled UV, manufacturing a phantom bump of up to
// the cell's world footprint on any sloped surface -- each coarse texel here is a
// depth-aware WEIGHTED AVERAGE of its footprint (the scheme of XeGTAO's depth MIP filter):
// samples near the footprint's closest depth get full weight and samples far behind it get
// little, so a distant background texel cannot drag a foreground cell's depth away, while
// the samples of a single continuous surface average smoothly. The result is an unbiased,
// prefiltered representation of the surface the march can read at any mip.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out float out_depth;

layout(binding = 0) uniform sampler2D u_input_depth;

layout(push_constant) uniform PushConstants {
    ivec2 src_size;
    int is_first_pass;
} u_push;

/// Weight floor for the footprint's farthest sample. Nonzero so a cell that genuinely
/// spans two surfaces still carries a trace of the far one (a hard 0 would degenerate the
/// filter back into min() at every silhouette), small so the near surface dominates.
const float kFarWeight = 0.1;

void main() {
    ivec2 frag_coord = ivec2(gl_FragCoord.xy);
    if (u_push.is_first_pass != 0) {
        out_depth = texelFetch(u_input_depth, frag_coord, 0).r;
        return;
    }

    // Conservative 3x3 footprint with edge clamping, for the same reason as
    // hiz_downsample.frag: at an odd source dimension a plain 2x2 never reaches the last
    // row/column, silently dropping it from every coarser mip.
    ivec2 base_coord = frag_coord * 2;
    float d[9];
    float d_min = 1.0;
    float d_max = 0.0;
    for (int dy = 0; dy < 3; ++dy) {
        for (int dx = 0; dx < 3; ++dx) {
            ivec2 c = min(base_coord + ivec2(dx, dy), u_push.src_size - 1);
            float v = texelFetch(u_input_depth, c, 0).r;
            d[dy * 3 + dx] = v;
            d_min = min(d_min, v);
            d_max = max(d_max, v);
        }
    }

    // Scale-free weighting: each sample's weight falls from 1 (at the footprint's nearest
    // depth) to kFarWeight (at its farthest) linearly in the footprint's own depth spread.
    // A near-flat footprint has negligible spread, so this degenerates to a plain average
    // of one surface; a silhouette footprint has a large spread, so the near surface
    // dominates -- occlusion-conservative without min()'s reconstruction bias.
    float spread = max(d_max - d_min, 1e-6);
    float sum  = 0.0;
    float wsum = 0.0;
    for (int i = 0; i < 9; ++i) {
        float w = 1.0 - (1.0 - kFarWeight) * (d[i] - d_min) / spread;
        sum  += d[i] * w;
        wsum += w;
    }
    out_depth = sum / wsum;
}
