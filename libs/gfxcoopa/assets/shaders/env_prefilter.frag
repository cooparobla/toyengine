#version 450

// GGX-prefilters mips 1..N-1 of the reflection probe cubemap from a real
// mip-0 capture (probe_capture.frag + probe_sky_background.frag write mip 0;
// this shader never writes it). Reads source_cube (mip 0 only, bound via
// CubemapTarget::mip0_cube_view()) instead of evaluating sky_gradient()
// closed-form, since mip 0 holds the captured scene geometry.

#include "cubemap_faces.glsl"

layout(set = 0, binding = 0) uniform samplerCube source_cube;

layout(location = 0) in  vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform PrefilterPC {
    int   face;       // cube array layer: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z
    float roughness;  // mip / (mip_levels - 1), in (0, 1] -- mip 0 is never dispatched here
} pc;

const float PI = 3.14159265359;
const uint  SAMPLE_COUNT = 64u;

// Hammersley / GGX importance sampling -- kept identical to brdf_lut.frag so
// the prefiltered cubemap and the BRDF LUT agree on what "roughness" means
// in the split-sum approximation.
float RadicalInverse_VdC(uint bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint N) {
    return vec2(float(i) / float(N), RadicalInverse_VdC(i));
}

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0 * PI * Xi.x;
    float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
    vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    vec3 up = abs(N.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
    vec3 T = normalize(cross(up, N));
    vec3 B = cross(N, T);
    return normalize(T * H.x + B * H.y + N * H.z);
}

void main() {
    vec3 N = cube_face_direction(pc.face, in_uv);

    // Split-sum prefilter assumes N == V == R, matching brdf_lut.frag.
    vec3 V = N;
    vec3 prefiltered = vec3(0.0);
    float total_weight = 0.0;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        vec2 Xi = Hammersley(i, SAMPLE_COUNT);
        vec3 H  = ImportanceSampleGGX(Xi, N, pc.roughness);
        vec3 L  = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            // textureLod(..., 0.0) rather than texture(...): L jumps
            // discontinuously between importance samples, so implicit-
            // derivative LOD selection would be meaningless. source_cube's
            // view also only exposes mip 0, so any nonzero LOD would be a
            // no-op anyway -- this is belt-and-braces.
            prefiltered  += textureLod(source_cube, L, 0.0).rgb * NdotL;
            total_weight += NdotL;
        }
    }

    out_color = vec4(prefiltered / max(total_weight, 1e-4), 1.0);
}
