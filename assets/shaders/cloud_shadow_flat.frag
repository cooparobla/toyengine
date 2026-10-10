#version 450

// The cloud shadow map for the flat (toon) clouds (CloudShadowPass): one texel per patch of the
// layer's base plane over a square around the camera, 0 under a cloud .. 1 in the clear, from the
// same field the layer draws (flat_cloud_field.glsl). The edge is soft over a little of the
// field's margin, so the shadows' outlines read as the clouds' puffs. Independent of the
// layer's opacity: a layer at opacity 0 is shadows only.

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_weather;
layout(set = 0, binding = 1) uniform sampler3D u_shape;

#define FLAT_UBO_SET 0
#define FLAT_UBO_BINDING 2
#include "flat_cloud_field.glsl"

layout(push_constant) uniform ShadowMapParams {
    vec4 map;     // xy = the map's corner (world), z = its side (world m), w = unused
    vec4 light;   // xyz = unit direction TO the light the scene is lit by, w = the map's resolution (texels)
} pc;

void main() {
    vec2 xy = pc.map.xy + (gl_FragCoord.xy / pc.light.w) * pc.map.z;
    out_color = vec4(1.0 - smoothstep(0.0, 0.08, flat_margin(xy)));
}
