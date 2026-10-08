#ifndef GFX_SURFACE_TRANSPARENT_TESC_GLSL
#define GFX_SURFACE_TRANSPARENT_TESC_GLSL

// gfx/surface/transparent_tesc.glsl -- the forward transparent pass's tessellation control stage
// (gfx/surface/tess_control.glsl). The view comes from the camera UBO (jitter_ndc.z = pixel
// scale): this pass has no surface-world set.

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
    mat4 prev_view;
    mat4 prev_proj;
    vec4 jitter_ndc;
} camera;

layout(push_constant) uniform PushConstants {
    vec4  albedo;
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;
    vec4  gfx_time;
    vec4  gfx_params;
    vec4  refraction_tint_thickness;
    vec4  refraction_ior_flags;
} material;

#define GFX_TESS_PACKED floatBitsToUint(material.refraction_ior_flags.zw)
#define GFX_TESS_VIEW vec4(camera.camera_pos, camera.jitter_ndc.z)
#define GFX_TESS_CLIP (camera.proj * camera.view)
#ifndef GFX_TESS_CULL_MARGIN
#define GFX_TESS_CULL_MARGIN 1.0
#endif
#include <gfx/surface/tess_control.glsl>

#endif // GFX_SURFACE_TRANSPARENT_TESC_GLSL
