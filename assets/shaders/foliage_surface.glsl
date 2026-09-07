// foliage_surface.glsl -- wind-swayed CUTOUT foliage. The only file with real code in the
// "foliage" derived shader; foliage.vert/foliage_shadow.vert/foliage_shadow_cube.vert are
// four-line includers of this file over three different backbones (see each entry point's
// own comment for the include-order contract). Compiling this ONE file into all three is
// what keeps a foliage card's shadow swaying in lockstep with its mesh -- see
// gfx/surface/shadow_vs.glsl's file doc for why that consistency is load-bearing rather
// than cosmetic.
//
// No fragment override: foliage reuses the stock gbuffer_fs.glsl/shadow_fs.glsl/
// shadow_cube_fs.glsl entry points verbatim (see the "foliage" SurfaceShaderDesc in
// engine.h, which leaves frag/shadow_frag/shadow_cube_frag empty) -- the CUTOUT alpha
// test and MRT writeout are already exactly what a leaf card needs.
//
// gfx_params (see PBRMaterial::shader_params):
//   x = wind_strength  -- world-space sway amplitude at the top of the card.
//   y = wind_frequency -- radians/second of the sway oscillation.
//   z = wind_dir.x, w = wind_dir.y -- horizontal sway direction (need not be normalized;
//       only its direction matters, the magnitude is folded into wind_strength).

void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    vec2 dir = gfx_params.zw;
    float dir_len = length(dir);
    if (dir_len < 1e-5) return; // no direction authored -- stay still rather than divide by ~0

    dir /= dir_len;

    // Phase varies with world-space position (not just time) so neighbouring cards don't
    // all sway in perfect unison -- a card at a different XY offset sees the same wave at a
    // different point in its cycle, like wind actually moving across a field.
    float phase = dot(v.position_ws.xy, dir) * 0.6 + gfx_time.x * gfx_params.y;

    // position_os.y is the card's local height axis (see plane.000's mesh data: a 2x2 quad
    // from y=-1 at the root to y=+1 at the tip, BEFORE this object's Transform rotation is
    // baked into the model matrix) -- smoothstep so the root (y<=0) stays planted and only
    // the upper half sways, growing to full amplitude at the tip.
    float mask = smoothstep(0.0, 1.0, v.position_os.y);

    v.position_ws.xy += dir * sin(phase) * gfx_params.x * mask;
}
