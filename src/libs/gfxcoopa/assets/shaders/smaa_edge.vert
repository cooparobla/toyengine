#version 450

#define SMAA_GLSL_4 1
#define SMAA_PRESET_CUSTOM 1
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 0

layout(push_constant) uniform SmaaEdgePush {
    vec4 rt_metrics;
    float threshold;
} push;

#define SMAA_RT_METRICS push.rt_metrics
#define SMAA_THRESHOLD push.threshold

#include "SMAA.hlsl"

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_offsets[3];

void main() {
    out_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(out_uv * 2.0 - 1.0, 0.0, 1.0);
    SMAAEdgeDetectionVS(out_uv, out_offsets);
}
