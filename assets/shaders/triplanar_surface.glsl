// triplanar_surface.glsl -- the `triplanar` vertex hook, shared by triplanar.vert and
// triplanar.tese: hands triplanar.frag the object-space position and normal plus the object's
// normal matrix (locations 6-10, after the backbone's frag_TBN).

layout(location = 6) out vec3 frag_object_pos;
layout(location = 7) out vec3 frag_object_normal;
layout(location = 8) flat out mat3 frag_normal_matrix; // locations 8-10

void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    frag_object_pos    = v.position_os;
    frag_object_normal = v.normal_os;
    frag_normal_matrix = v.normal_matrix;
}
