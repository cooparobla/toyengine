#ifndef TOY_CLOUD_SHADOW_GLSL
#define TOY_CLOUD_SHADOW_GLSL

// cloud_shadow.glsl -- the clouds' shadow on a point (render cloud_shadows): how much of the
// directional light reaches it through the cloud layer, 1 = all of it.
//
// The cloud shadow map (CloudShadowPass) holds the light's transmittance through the layer at
// each point of the layer's base plane, over a square around the camera. A point looks it up
// where its ray toward the light crosses that plane. Fades out across the map's outer tenth (so
// the shadowed square has no edge), as the light sinks low (`layer.z`, set by the CPU per cloud
// type: a grazing light smears the shadows into long streaks, and across a wall the lookup would
// sweep the map in bands), and for a point risen into the layer itself (its light no longer
// crosses all of it).
//
// The caller samples the sampler2D `cloud_shadow_map` (the shadow set's binding 4) itself.
//
// `params`: xy = the map's corner (world), z = 1 / its side, w = strength (0 = off).
// `layer`:  x = the layer's base height (world z), y = its thickness, z = the low-light fade.
// `L`:      unit direction TO the light.
//
// The lookup's uv and weight (cloud_shadow_lookup) are split from the read itself, which callers
// do inline: textureLod(cloud_shadow_map, uv, 0.0).r. A function that samples a global from inside
// a deeper call chain trips SPIRV-Cross's MSL argument passing in the froxel volumetrics shader.
// `weight` is 0 when the point takes no cloud shadow (then `uv` is irrelevant).
void cloud_shadow_lookup(vec4 params, vec4 layer, vec3 L, vec3 P, out vec2 uv, out float weight) {
    uv = vec2(0.5);
    weight = 0.0;
    if (params.w <= 0.0 || L.z < 0.02) return;
    vec2 q = P.xy + L.xy * ((layer.x - P.z) / L.z);
    uv = (q - params.xy) * params.z;
    vec2 e = min(uv, 1.0 - uv);
    float edge = smoothstep(0.0, 0.1, min(e.x, e.y));
    float inside = 1.0 - clamp((P.z - layer.x) / max(layer.y, 1e-3), 0.0, 1.0);
    weight = params.w * edge * inside * layer.z;
}

#endif // TOY_CLOUD_SHADOW_GLSL
