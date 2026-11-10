#version 450

#define SMAA_GLSL_4 1
#define SMAA_PRESET_CUSTOM 1
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1

layout(push_constant) uniform SmaaEdgePush {
    vec4 rt_metrics;
    float threshold;
} push;

#define SMAA_RT_METRICS push.rt_metrics
#define SMAA_THRESHOLD push.threshold

#include "SMAA.hlsl"

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_offsets[3];

layout(set = 0, binding = 0) uniform sampler2D color_sampler;

layout(location = 0) out vec2 out_edges;

void main() {
    out_edges = SMAAColorEdgeDetectionPS(in_uv, in_offsets, color_sampler);
}
