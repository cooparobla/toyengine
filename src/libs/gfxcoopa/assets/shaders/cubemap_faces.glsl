// cubemap_faces.glsl -- shared cube-face direction reconstruction.
//
// Exact inverse of the Vulkan cubemap (sc, tc, ma) face-selection convention
// (Vulkan spec, "Cube Map Face Selection"), for face layer order
// +X,-X,+Y,-Y,+Z,-Z (matching CubemapTarget's array-layer order). `uv` is the
// framebuffer's normalized texel coordinate for this (face, mip) render
// target, which maps 1:1 onto the (s, t) a later samplerCube lookup will
// produce -- there is NO Y-flip here. (A skybox shader that reconstructs
// directions through camera.proj must negate NDC.y for the Vulkan Y-flip;
// this path has no projection matrix at all.)
//
// This is the single source of truth for cube-face orientation: it must stay
// in lockstep with CubemapTarget::get_face_view()/get_face_projection()
// (gfxcoopa/engine/targets/cubemap_target.h), which were derived specifically
// to reproduce these formulas when rendering real geometry into the cubemap.
vec3 cube_face_direction(int face, vec2 uv) {
    vec2 c = uv * 2.0 - 1.0; // c.x -> sc, c.y -> tc
    vec3 d;
    if      (face == 0) d = vec3( 1.0, -c.y, -c.x); // +X
    else if (face == 1) d = vec3(-1.0, -c.y,  c.x); // -X
    else if (face == 2) d = vec3( c.x,  1.0,  c.y); // +Y
    else if (face == 3) d = vec3( c.x, -1.0, -c.y); // -Y
    else if (face == 4) d = vec3( c.x, -c.y,  1.0); // +Z
    else                 d = vec3(-c.x, -c.y, -1.0); // -Z
    return normalize(d);
}
