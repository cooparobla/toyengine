#version 450

// `terrain_styled` / `terrain_styled_planet` vertex stage: carries the packed surface-kind
// code (toy::world::encode_surface_blend(), tile_topology.h; TileMeshLibrary::append_styled()
// writes it into every stamped vertex's uv.x) through its OWN flat varying instead of the
// backbone's smoothly-interpolated uv.
//
// The code is a bit-packed integer, the SAME value at every vertex of a stamp ("one code per
// stamp: corners weld" -- tile_mesh_library.cpp's append_styled()). In exact arithmetic it
// therefore interpolates as a constant, but frag_uv is an ordinary smoothly-interpolated
// varying, and GPU perspective-correct interpolation divides by an interpolated 1/w -- which,
// on a distant, near-grazing triangle, can vary enough corner to corner to push the
// interpolated value more than 0.5 off the intended integer. styled_kind() (terrain_styled.glsl)
// rounds that back to an int: a value that lands on the wrong side decodes to a different,
// essentially random surface kind, each pointing at a different atlas swatch colour -- distant
// soft-tile terrain reading as colour speckle that is static in a still frame (the rounding is
// deterministic per pixel) and shifts under camera motion (the interpolation does). A `flat`
// varying (gbuffer_vs.glsl already carries one, frag_snow_up) sidesteps interpolation and its
// precision loss entirely -- it is also the semantically correct qualifier, since this is
// categorical data, not a continuously-varying quantity.
#define GFX_SURFACE_VERTEX
#include <gfx/surface/gbuffer_vs.glsl>

layout(location = 6) flat out float frag_blend_code;

void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    frag_blend_code = v.uv.x;
}
