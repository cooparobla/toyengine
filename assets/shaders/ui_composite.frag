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
// upscale happens. `ui` is built at the destination rect itself, so the
// same sampler and UVs give an exact texel-for-texel fetch -- the UI is never
// resampled; building it at render resolution would force a non-integer upscale
// that makes world-canvas text look stepped.
//
// No sRGB decode/encode here, unlike upscale.frag: both sources are UNORM images
// already holding the display-referred, sRGB-ENCODED bytes stylize.frag
// produced, and the destination is UNORM too -- so this blend happens in that
// encoded space. upscale.frag's decode cancels the swapchain's implicit encode
// one stage later.

layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D base_sampler;  // post-processed scene
layout(set = 0, binding = 1) uniform sampler2D ui_sampler;    // world UI layer, PREMULTIPLIED
layout(set = 0, binding = 2) uniform sampler2D blur_sampler;  // the same scene, blurred (glass backdrop)

layout(push_constant) uniform Push {
    float scene_blur;   // 0 = the sharp frame, 1 = fully blurred
    float _pad0;
    float _pad1;
    float _pad2;
} pc;

layout(location = 0) out vec4 out_color;

void main() {
    vec3 base = texture(base_sampler, in_uv).rgb;
    vec4 ui   = texture(ui_sampler,   in_uv);

    // "The interface is open, so the world behind it goes soft" -- a cross-fade toward the
    // blurred copy the glass panes already sample (ToyRenderConfig::ui_scene_blur), not a
    // second blur with a ramping radius: one mix here is free, and it reaches the same place.
    // Both images are the same scene in the same encoded space, so mixing them needs no
    // gamma round trip. Branched, not just mixed by 0, so the common case does not pay for
    // the blurred fetch at all.
    if (pc.scene_blur > 0.0) {
        base = mix(base, texture(blur_sampler, in_uv).rgb, clamp(pc.scene_blur, 0.0, 1.0));
    }

    // Premultiplied "over". The layer is premultiplied because BlendMode::AlphaOver
    // wrote `src.rgb * src.a` into an alpha-0-cleared target; mix(base, ui.rgb, ui.a)
    // would multiply by alpha a second time and halve a 50%-alpha panel's colour.
    out_color = vec4(ui.rgb + base * (1.0 - ui.a), 1.0);
}
