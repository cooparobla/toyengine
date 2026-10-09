#version 450

// Physical sky, LUT 1 of 3: transmittance from any height along any direction to the top of the
// atmosphere (256 x 64, Bruneton's parameterisation -- see sky_atmosphere.glsl). Rendered by
// SkyAtmospherePass only when the atmosphere's parameters change.

#include "sky_atmosphere.glsl"

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform Params {
    SkyAtmosphere atmo;
} pc;

void main() {
    vec2 uv = vec2(sky_from_sub_uv_to_unit(in_uv.x, SKY_TRANSMITTANCE_RES.x),
                   sky_from_sub_uv_to_unit(in_uv.y, SKY_TRANSMITTANCE_RES.y));
    float r, mu;
    sky_transmittance_params(pc.atmo, clamp(uv, 0.0, 1.0), r, mu);

    vec3 pos = vec3(0.0, 0.0, r);
    vec3 dir = vec3(sqrt(max(1.0 - mu * mu, 0.0)), 0.0, mu);
    float t_max = sky_ray_sphere(pos, dir, pc.atmo.ground.w);
    if (t_max < 0.0) t_max = 0.0;

    const int STEPS = 40;
    vec3 depth = vec3(0.0);
    float dt = t_max / float(STEPS);
    for (int i = 0; i < STEPS; ++i) {
        vec3 p = pos + dir * ((float(i) + 0.5) * dt);
        depth += sky_sample_medium(pc.atmo, length(p)).extinction * dt;
    }
    out_color = vec4(exp(-depth), 1.0);
}
