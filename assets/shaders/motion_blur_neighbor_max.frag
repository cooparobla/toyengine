#version 450

// motion_blur_neighbor_max.frag -- motion blur stage 2 (toyengine/render/passes/motion_blur_pass.h).
//
// The longest TileMax vector within `reach` tiles in every direction (McGuire's "NeighborMax"): the
// dominant motion that can smear INTO any pixel of this tile. The reach is ceil(max radius / tile),
// so a blur longer than one tile still finds the object it comes from.

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_max;

layout(set = 0, binding = 0) uniform sampler2D tex_tile_max;   // nearest

layout(push_constant) uniform NeighborParams {
    ivec4 info;   // x = reach (tiles), yz = tile grid size
} pc;

void main() {
    const ivec2 t     = ivec2(gl_FragCoord.xy);
    const ivec2 grid  = pc.info.yz;
    const int   reach = pc.info.x;

    vec2  best     = vec2(0.0);
    float best_len = 0.0;
    for (int y = -reach; y <= reach; ++y) {
        for (int x = -reach; x <= reach; ++x) {
            ivec2 q = clamp(t + ivec2(x, y), ivec2(0), grid - 1);
            vec2  v = texelFetch(tex_tile_max, q, 0).xy;
            float l = dot(v, v);
            if (l > best_len) { best_len = l; best = v; }
        }
    }
    out_max = vec4(best, 0.0, 1.0);
}
