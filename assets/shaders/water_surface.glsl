// water_surface.glsl -- Gerstner-ish wave displacement plus animated ripple normals. The
// only file with real code in the "water" derived shader; water.vert/water.frag are
// four-line includers of this file over the transparent vertex/fragment backbones (see
// each entry point's own comment). No shadow entry points: water is BLEND (see the
// "water" SurfaceShaderDesc in engine.h, SurfaceShaderDomain::Transparent), and BLEND
// materials only cast a shadow at full opacity (see record_directional_shadow_'s doc) --
// water is never opaque, so it never reaches the shadow pass at all.
//
// Reuses the transparent backbone's existing refraction/SSR/Beer-Lambert absorption
// end to end (see PBRMaterial::refraction on the water scene entry) -- this file only
// adds the animation those already-working systems shade.
//
// gfx_params (see PBRMaterial::shader_params):
//   x = wave_amplitude -- world-space vertical displacement at the wave crest.
//   y = wave_speed     -- world units/second the wave pattern travels.
//   z = wave_dir.x, w = wave_dir.y -- horizontal wave travel direction (need not be
//       normalized; only its direction matters).

// Guarded by GFX_SURFACE_VERTEX/GFX_SURFACE_FRAGMENT (not just presence in this file): this
// file is included by BOTH water.vert (which declares GfxSurfaceVertex, not
// GfxTransparentSurface) and water.frag (the reverse), so each function must compile out
// entirely in the stage that doesn't declare its parameter type -- without this guard,
// water.vert would fail to compile referencing the undeclared GfxTransparentSurface, and
// vice versa in water.frag.
#ifdef GFX_SURFACE_VERTEX
void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    vec2 dir = gfx_params.zw;
    float dir_len = length(dir);
    if (dir_len < 1e-5) return;
    dir /= dir_len;

    // A single sine wave is "Gerstner-ish", not a full Gerstner sum (no horizontal
    // displacement, no multi-wave superposition) -- sufficient to prove the architecture
    // (the displacement mechanism itself doesn't care how many waves get summed here) and
    // cheap enough to read the derivative of in closed form for the normal below.
    float k     = 1.6;                          // spatial frequency, world units^-1
    float phase = dot(v.position_ws.xy, dir) * k + gfx_time.x * gfx_params.y;
    v.position_ws.z += sin(phase) * gfx_params.x;

    // Tilt the normal along the wave's slope (d/dphase of the height field above, times
    // dphase/d(world pos)) so the surface visibly catches light along the crests instead
    // of shading as a flat plane with a wavy silhouette -- cheap and exact for this single
    // sine wave, unlike foliage's displacement (which never touches the normal, since a
    // swaying flat card has no meaningful curvature to shade).
    float slope = cos(phase) * gfx_params.x * k;
    v.normal_ws = normalize(v.normal_ws - vec3(dir * slope, 0.0));
}
#endif // GFX_SURFACE_VERTEX

#ifdef GFX_SURFACE_FRAGMENT
void gfx_surface_fragment(inout GfxTransparentSurface s) {
    // A second, faster/finer ripple layer on top of the vertex wave's coarse slope --
    // exactly what a fragment-stage hook is for for (per-pixel detail no vertex density
    // here would resolve), independent of gfx_surface_vertex()'s displacement.
    vec2 dir = normalize(gfx_params.zw + vec2(0.3, -0.2)); // offset so the two layers cross, not stack
    float phase = dot(s.position_ws.xy, dir) * 4.0 + gfx_time.x * gfx_params.y * 1.7;
    float ripple = sin(phase) * 0.15;
    s.normal_ws = normalize(s.normal_ws + vec3(dir * ripple, 0.0));
}
#endif // GFX_SURFACE_FRAGMENT
