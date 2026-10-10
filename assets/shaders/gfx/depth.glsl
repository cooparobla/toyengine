#ifndef GFX_DEPTH_GLSL
#define GFX_DEPTH_GLSL

// gfx/depth.glsl -- shared depth-linearization helper (the same formula as
// stylize.frag's local `linear_depth()`), so the DoF stages (via
// gfx/dof_common.glsl), particle.frag and temporal_history.frag use one copy
// rather than carrying their own. See toy_shadow_body.glsl
// doc for this codebase's general policy on de-duplicating shared shader bodies.

// True view-space distance from the camera, from raw Vulkan [0,1] post-projection
// depth. Perspective depth is hyperbolic (glm::perspective -> perspectiveRH_ZO);
// orthographic depth is already linear in the raw value (glm::ortho ->
// orthoRH_ZO), hence the branch.
float gfx_linear_depth(float d, float near_z, float far_z, float is_perspective) {
    if (is_perspective < 0.5) {
        return mix(near_z, far_z, d);
    }
    return near_z * far_z / (far_z - d * (far_z - near_z));
}

#endif // GFX_DEPTH_GLSL
