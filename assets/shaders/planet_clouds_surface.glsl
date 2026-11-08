// planet_clouds_surface.glsl -- a cloud deck as a sphere shell just outside the ground, inside
// the planet's atmosphere (a raymarched VolumeComponent, VolumeShape::Shell -- see
// toyengine/world/planet_atmosphere.h -- not a mesh shader). gfx_surface_vertex() and
// gfx_surface_fragment() in the Transparent backbone (gfx/surface/transparent_fs.glsl).
//
// The coverage field is evaluated in the shell's own OBJECT-space direction (passed vertex ->
// fragment through GFX_SURFACE_CUSTOM_VARYING), not world space, so it rotates with the mesh
// rather than swimming if the shell is ever spun independently of the planet. Self-occlusion by
// the solid ground is ordinary depth testing (BLEND geometry is depth-tested against the opaque
// scene) -- nothing extra to compute there; the only shell-specific test is the same
// front/back-face rule the atmosphere shell uses, so the dome's far side does not double-draw
// through its own near side when the camera is outside it.
//
// gfx_params:      x = shell radius (world units, matches the mesh)
//                  y = noise scale (spatial frequency of the coverage field)
//                  z = coverage, 0..1 (higher = more of the sky covered)
//                  w = drift speed (shape evolves; the whole layer does not rotate the mesh)
// gfx_params_ext0:  xyz = planet centre, world space (w unused)
// gfx_params_ext1:  xyz = sunlit tint, linear rgb (w unused)

#ifdef GFX_SURFACE_VERTEX
void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    v.custom = vec4(v.position_os, 0.0);
}
#endif

#ifdef GFX_SURFACE_FRAGMENT

float planet_clouds_hash(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

float planet_clouds_noise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = planet_clouds_hash(i + vec3(0.0, 0.0, 0.0));
    float n100 = planet_clouds_hash(i + vec3(1.0, 0.0, 0.0));
    float n010 = planet_clouds_hash(i + vec3(0.0, 1.0, 0.0));
    float n110 = planet_clouds_hash(i + vec3(1.0, 1.0, 0.0));
    float n001 = planet_clouds_hash(i + vec3(0.0, 0.0, 1.0));
    float n101 = planet_clouds_hash(i + vec3(1.0, 0.0, 1.0));
    float n011 = planet_clouds_hash(i + vec3(0.0, 1.0, 1.0));
    float n111 = planet_clouds_hash(i + vec3(1.0, 1.0, 1.0));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

float planet_clouds_fbm(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int o = 0; o < 4; ++o) {
        v += a * planet_clouds_noise(p);
        p *= 2.03;
        a *= 0.5;
    }
    return v;
}

void gfx_surface_fragment(inout GfxTransparentSurface s) {
    vec3  centre   = gfx_params_ext0.xyz;
    float radius   = max(gfx_params.x, 1.0);
    float scale    = max(gfx_params.y, 1e-3);
    float coverage = clamp(gfx_params.z, 0.0, 1.0);
    float speed    = gfx_params.w;
    vec3  tint     = gfx_params_ext1.xyz;

    vec3 ro = camera.camera_pos - centre;
    bool outside = length(ro) > radius;
    if (outside && !gl_FrontFacing) {
        s.alpha = 0.0;
        return;
    }

    vec3 local_dir = normalize(s.custom.xyz);
    vec3 p = local_dir * scale + vec3(gfx_time.x * speed, 0.0, 0.0);
    float n = planet_clouds_fbm(p);
    float density = smoothstep(1.0 - coverage, 1.0, n);

    // Lambert against the sun, in the shell's own outward normal: the terminator falls where
    // the ground's would, and the night side of the deck goes dark rather than floating lit.
    vec3  N   = normalize(s.position_ws - centre);
    vec3  sun = lights.sky_sun.xyz;
    float ndl = clamp(dot(N, sun), 0.0, 1.0);
    vec3  lit = mix(tint * 0.1, tint, ndl);

    s.albedo    = vec3(0.0);
    s.roughness = 1.0;
    s.alpha     = density * 0.9;
    s.emission  = lit * density;
}

#endif
