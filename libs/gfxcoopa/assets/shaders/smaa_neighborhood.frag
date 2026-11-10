#version 450

#define SMAA_GLSL_4 1
#define SMAA_PRESET_CUSTOM 1
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1

layout(push_constant) uniform SmaaNeighborhoodPush {
    vec4  rt_metrics;
    float exposure;
} push;

#define SMAA_RT_METRICS push.rt_metrics
#define SMAA_THRESHOLD 0.05
#define SMAA_MAX_SEARCH_STEPS 32
#define SMAA_MAX_SEARCH_STEPS_DIAG 16
#define SMAA_CORNER_ROUNDING 25

#include "SMAA.hlsl"

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_offset;

layout(set = 0, binding = 0) uniform sampler2D color_sampler;
layout(set = 0, binding = 1) uniform sampler2D blend_sampler;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = SMAANeighborhoodBlendingPS(in_uv, in_offset, color_sampler, blend_sampler);
}
