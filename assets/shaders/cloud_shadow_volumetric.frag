#version 450

// The cloud shadow map for the volumetric clouds (CloudShadowPass): one texel per patch of the
// layer's base plane over a square around the camera, holding how much of the sun's light gets
// through the layer above it -- a march toward the light through the same density field the
// view march draws (sky_cloud_density.glsl, without the detail erosion: its effect on optical
// depth over the slab is noise). No jitter: the map is smooth and still, so the shadows it casts
// can never flicker. cloud_shadow.glsl reads it for every surface, the water and the fog.

#define CLOUD_UBO_SET 0
#define CLOUD_UBO_BINDING 3
#include "sky_cloud_common.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D u_weather;
layout(set = 0, binding = 1) uniform sampler3D u_shape;
layout(set = 0, binding = 2) uniform sampler3D u_detail;   // unread (no detail erosion); the density code declares it

#include "sky_cloud_density.glsl"

layout(push_constant) uniform ShadowMapParams {
    vec4 map;     // xy = the map's corner (world), z = its side (world m), w = march steps
    vec4 light;   // xyz = unit direction TO the light the scene is lit by, w = the map's resolution (texels)
} pc;

void main() {
    float scale = cf.look.x;
    vec3 L = pc.light.xyz;
    if (L.z < 0.02) { out_color = vec4(1.0); return; }

    // This texel's point on the layer's base (cloud space), on the same planet-curved shell the
    // view march uses (centred under the camera).
    vec2 xy = (pc.map.xy + (gl_FragCoord.xy / pc.light.w) * pc.map.z) / scale;
    vec3 cam = cf.camera.xyz / scale;
    g_center = vec3(cam.xy, -PLANET_R);
    float r_base = PLANET_R + cf.slab.x;
    vec2 dxy = xy - cam.xy;
    float z = sqrt(max(r_base * r_base - dot(dxy, dxy), 0.0)) - PLANET_R;
    vec3 p0 = vec3(xy, z);

    // Up through the slab toward the light (capped at a few thicknesses for a low sun).
    float len = cf.slab.y / max(L.z, 0.25);
    int steps = int(pc.map.w);
    float dl = len / float(steps);
    float foot = max(pc.map.z / pc.light.w / scale, dl * 0.25);
    float od = 0.0;
    for (int i = 0; i < steps; ++i) {
        vec3 p = p0 + L * (dl * (float(i) + 0.5));
        bool e;
        // A long step length softens the field's surface (sky_cloud_density.glsl's CONTRAST): the
        // shadows' edges are a soft penumbra a few texels wide rather than a texel staircase.
        od += cloud_density(p, layer_height(p), foot, max(dl, 200.0), false, e) * dl;
    }
    out_color = vec4(exp(-od * CLOUD_SIGMA));
}
