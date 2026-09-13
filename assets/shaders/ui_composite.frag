#version 450

// Composites the low-resolution world-space UI layer over the finished,
// post-processed frame -- the stage that keeps UI out of DOF, AA and tilt shift.
//
// The UI is drawn into its own RGBA8 layer (ui_world_target_, cleared to a fully
// transparent black) rather than into post_target_ alongside the scene, so that
// every display-space effect downstream of post_target_ -- FXAA/SMAA/TAA and
// TiltShiftPass -- has already run by the time this shader puts the UI on top.
//
// `base` is whatever that chain produced, still at render resolution when tilt
// shift is off, so sampling it NEAREST at destination UVs is where the scene's
// pixel-art upscale happens. `ui` is built at the destination rect itself, so the
// same sampler and UVs give an exact texel-for-texel fetch -- the UI is never
// resampled. It used to share the base's render resolution, and the non-integer
// upscale that implied is what made world-canvas text look stepped.
//
// No sRGB decode/encode here, unlike upscale.frag: both sources are UNORM images
// already holding the display-referred, sRGB-ENCODED bytes pixel_stylize.frag
// produced, and the destination is UNORM too -- so this blend happens in the same
// encoded space the blend inside post_target_ used to. upscale.frag's decode
// still cancels the swapchain's implicit encode one stage later, unchanged.

layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D base_sampler;  // post-processed scene
layout(set = 0, binding = 1) uniform sampler2D ui_sampler;    // world UI layer, PREMULTIPLIED

layout(location = 0) out vec4 out_color;

void main() {
    vec3 base = texture(base_sampler, in_uv).rgb;
    vec4 ui   = texture(ui_sampler,   in_uv);

    // Premultiplied "over". The layer is premultiplied because BlendMode::AlphaOver
    // wrote `src.rgb * src.a` into an alpha-0-cleared target; mix(base, ui.rgb, ui.a)
    // would multiply by alpha a second time and halve a 50%-alpha panel's colour.
    out_color = vec4(ui.rgb + base * (1.0 - ui.a), 1.0);
}
