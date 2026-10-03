// editor_shading.glsl -- the editor's viewport shading looks (Blender's Solid / Material
// Preview), shared by debug_view.frag (opaque G-buffer surfaces) and
// transparent_preview.frag (BLEND materials drawn over it), so glass is lit exactly like the
// surfaces behind it. Lighting-independent: a scene with no lights reads the same.
// Pure functions -- no bindings.

#ifndef EDITOR_SHADING_GLSL
#define EDITOR_SHADING_GLSL

// Screen-space backdrop gradients (v = 0 at the top of the view).
vec3 editor_solid_backdrop(float v)  { return mix(vec3(0.24, 0.25, 0.27), vec3(0.15, 0.16, 0.18), v); }
vec3 editor_matprev_backdrop(float v) { return mix(vec3(0.33, 0.34, 0.36), vec3(0.20, 0.21, 0.23), v); }

// Solid: a headlight plus a fixed key and fill, over a light grey lightly tinted by albedo.
vec3 editor_solid(vec3 N, vec3 world_pos, vec3 camera_pos, vec3 albedo) {
    vec3 Vh = normalize(camera_pos - world_pos);
    float head = max(dot(N, Vh), 0.0);
    float key  = max(dot(N, normalize(vec3(0.35, 0.45, 0.82))), 0.0);
    float fill = max(dot(N, normalize(vec3(-0.6, -0.3, 0.2))), 0.0);
    float shade = 0.22 + 0.55 * head + 0.33 * key + 0.10 * fill;
    return mix(vec3(0.82), albedo, 0.35) * shade;
}

// Material Preview: the authored material under a fixed studio rig -- key and rim lights,
// hemispherical ambient (+Z up) and a soft "softbox" reflection. `occlusion` multiplies the
// ambient terms (material AO x screen-space AO).
vec3 editor_material_preview(vec3 N, vec3 world_pos, vec3 camera_pos, vec3 albedo, float metallic,
                             float roughness, float occlusion, vec3 emissive) {
    float rough = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);
    vec3 V = normalize(camera_pos - world_pos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 diff_col = albedo * (1.0 - metallic);
    float NoV = max(dot(N, V), 1e-3);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0) * (1.0 - rough);
    vec3 sky = vec3(0.80, 0.82, 0.86), ground = vec3(0.22, 0.21, 0.20);
    vec3 amb = mix(ground, sky, N.z * 0.5 + 0.5);
    vec3 R = reflect(-V, N);
    vec3 env = mix(ground, sky * 1.15, smoothstep(-0.2, 0.6, R.z));
    env = mix(env, amb, rough);
    vec3 col = (diff_col * amb * 0.75 + env * F * (1.0 - 0.5 * rough)) * occlusion;
    const vec3 Ls[2] = vec3[2](normalize(vec3(0.45, -0.55, 0.70)), normalize(vec3(-0.6, 0.5, 0.35)));
    const float Is[2] = float[2](1.25, 0.45);
    for (int i = 0; i < 2; ++i) {
        vec3 L = Ls[i];
        vec3 H = normalize(L + V);
        float NoL = max(dot(N, L), 0.0), NoH = max(dot(N, H), 0.0);
        float a2 = rough * rough * rough * rough;
        float d = NoH * NoH * (a2 - 1.0) + 1.0;
        float D = a2 / (3.14159 * d * d);
        float k = (rough + 1.0) * (rough + 1.0) / 8.0;
        float G = NoL / (NoL * (1.0 - k) + k) * NoV / (NoV * (1.0 - k) + k);
        vec3 Fs = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        vec3 spec = D * G * Fs / max(4.0 * NoL * NoV, 1e-3);
        col += (diff_col / 3.14159 * (1.0 - Fs) + spec) * NoL * Is[i] * 2.4 * mix(1.0, occlusion, 0.5);
    }
    col += emissive;
    return col / (1.0 + col * 0.35);   // gentle rolloff so highlights don't clip
}

#endif
