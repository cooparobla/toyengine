#version 450

#define SMAA_GLSL_4 1
#define SMAA_PRESET_CUSTOM 1
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1

layout(push_constant) uniform SmaaBlendPush {
    vec4 rt_metrics;
    int  max_search_steps;
} push;

#define SMAA_RT_METRICS push.rt_metrics
#define SMAA_MAX_SEARCH_STEPS push.max_search_steps
#define SMAA_MAX_SEARCH_STEPS_DIAG 16
#define SMAA_CORNER_ROUNDING 25

#include "SMAA.hlsl"

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec2 in_pixcoord;
layout(location = 2) in vec4 in_offsets[3];

layout(set = 0, binding = 0) uniform sampler2D edges_sampler;
layout(set = 0, binding = 1) uniform sampler2D area_sampler;
layout(set = 0, binding = 2) uniform sampler2D search_sampler;

layout(location = 0) out vec4 out_weights;

void main() {
    vec4 subsampleIndices = vec4(0.0);
    out_weights = SMAABlendingWeightCalculationPS(in_uv, in_pixcoord, in_offsets, edges_sampler, area_sampler, search_sampler, subsampleIndices);
}
