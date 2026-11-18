#version 450

// Stylize overlay: optional bloom composite -> exposure+ACES tonemap ->
// colour grading -> luminance-keyed crosshatch -> depth/normal-discontinuity
// outline (alpha-blended over the input color) -> ordered (Bayer) dither ->
// palette quantization, in that order. Bloom runs first (and in linear HDR)
// since it's an additive light contribution, not a stylistic filter over the
// final image the way the rest are. The hatch runs before the outline
// because a pen shades a region first and inks its contour over that -- the
// reverse order would let strokes cut the contour into dashes. Paper grain
// is not a stage of its own: it modulates the hatch's and the outline's ink
// COVERAGE (the paper's tooth decides where a stroke skips or pools), so
// bare paper the pen never touched is never perturbed on its way into the
// dither/palette steps below.
//
// The tonemap step is optional: a full-PBR renderer with its own tonemap/AA
// chain (e.g. blendy) leaves `exposure` <= 0 and feeds this shader
// already-tonemapped LDR input, so nothing double-tonemaps. A consumer with
// no tonemap step of its own (e.g. toyengine) sets `exposure` > 0 and feeds
// raw HDR scene color instead.
//
// All eight effects are independently no-ops at their "off" value
// (bloom_intensity <= 0, exposure <= 0, outline_thickness <= 0,
// dither_strength <= 0, palette_count <= 0, hatch_strength <= 0,
// paper_strength <= 0, outline_wobble <= 0), so a consumer can leave this
// pass always-constructed and always-executed and simply zero the fields it
// doesn't want. One field is shared rather than independent: outline_color
// is read by the hatch stage as well as the outline stage (hatching and the
// ink contour are the same pen), so hatch_strength > 0 draws ink-coloured
// strokes even when outline_thickness == 0.

layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D scene_color;  // already tonemapped LDR
layout(set = 0, binding = 1) uniform sampler2D scene_depth;  // gbuffer depth
layout(set = 0, binding = 2) uniform sampler2D scene_normal; // gbuffer world normal (+ metallic in .a)
layout(set = 0, binding = 3) uniform sampler2D palette_lut;  // Nx1 RGBA8, NEAREST
// Finished bloom image from BloomPass: already bright-pass filtered and blurred
// through a downsample/upsample pyramid, at HALF this pass's resolution. Sampled
// with a plain texture() through a LINEAR sampler -- the bilinear upsample to full
// resolution is free and is exactly what a soft additive glow wants.
layout(set = 0, binding = 4) uniform sampler2D bloom_tex;
// 1x1 adapting exposure multiplier from ExposurePass, or a 1x1 texture of 1.0
// when auto-exposure is off -- so the read needs no separate flag.
layout(set = 0, binding = 5) uniform sampler2D exposure_tex;
// Colour-grading strip LUT (see GradingLut / gfx/grading.glsl); grading_size <= 1
// disables the lookup, and the binding then points at a 1x1 dummy.
layout(set = 0, binding = 6) uniform sampler2D grading_lut;

// Per-material outline mask (outline_mode != 0): the G-buffer's emissive attachment, whose
// alpha carries each material's packed toon code -- bit 10 is "outline this material" (see
// PBRMaterial::gpu_toon_code()). NEAREST, like depth/normal above: a packed integer code
// must not be blended across a material boundary. Bound to a harmless stand-in and never
// read when outline_mode is 0 (see StylizePass::set_source_images()).
layout(set = 0, binding = 7) uniform sampler2D outline_mask;

// G-buffer world position (G2.rgb; .a is roughness, unread here) -- the stroke-plane
// coordinate for hatch_space == surface. NEAREST, like depth/normal/outline_mask above: a
// blended position across a silhouette is a point on neither surface. Bound
// unconditionally (it is always a real, rendered attachment, like binding 7), and
// hatch_space is what decides whether it is read.
layout(set = 0, binding = 8) uniform sampler2D g_world_pos;

// The transparent pass's coverage attachment: the RECIPROCAL of the camera distance of the
// nearest transparent surface, 0 where there is none. Reciprocal because the attachment is
// written with BlendMode::Max, which then resolves "nearest" whatever order the transparents
// drew in -- and which is why the 0 clear means "nothing here" rather than "at the camera".
// NEAREST, like bindings 1/2/7/8: a value blended across the water's edge is a distance to
// neither side. Read only when outline_through_water > 0.
layout(set = 0, binding = 9) uniform sampler2D transparent_coverage;

#include <gfx/grading.glsl>
#include <gfx/noise.glsl>

layout(push_constant) uniform StylizePushConstants {
    vec4  outline_color;      // listed first: std430 would otherwise pad around a mid-struct vec4
    vec2  inv_render_size;
    float outline_thickness;  // line width in texels, rounded to whole steps; <= 0 disables
    float depth_threshold;
    float normal_threshold;
    float dither_strength;    // <= 0 disables
    float palette_count;      // <= 0 disables palette quantization
    float camera_near;
    float camera_far;
    float camera_is_perspective; // >= 0.5 => perspective, else orthographic
    float exposure;              // <= 0 disables the tonemap step (input is already LDR)
    float bloom_intensity;       // <= 0 disables bloom
    float auto_exposure;         // != 0 -> multiply `exposure` by exposure_tex's adapting value
    float grading_size;          // colour-grading LUT cube side; <= 1 disables the lookup
    float outline_mode;          // 0 = outline every edge; 1 = only materials that asked (bit 10 of
                                 //     the toon code at `outline_mask`)
    float hatch_strength;        // <= 0 disables the crosshatch stage
    float hatch_space;           // < 0.5 = screen-locked strokes, >= 0.5 = world-locked (surface)
    float hatch_scale;           // stroke period, in render pixels; clamped to >= 2
    float hatch_angle_deg;       // layer 0's angle; layers 1/2 are offset internally
    float hatch_layers;          // rounded and clamped to 1..3
    float hatch_threshold;       // display-referred luma at/above which nothing hatches
    float hatch_softness;        // extra stroke-edge AA, in multiples of one pixel
    float paper_strength;        // fraction of ink coverage the paper's tooth perturbs; <= 0 disables it
    float outline_wobble;        // ink-contour displacement, in texels; <= 0 disables it
    float outline_focal_px;      // render pixels per world unit at unit depth; <= 0 disables
                                 //     the crease fade below (nothing else reads it)
    float outline_crease_fade;   // world units one texel may span before interior creases stop
                                 //     inking; <= 0 keeps every crease at full strength
    float hatch_pbr_threshold;   // hatch_threshold for NON-toon-banded surfaces only; <= 0 gives
                                 //     every surface the one hatch_threshold
    float outline_through_water; // fraction of the contour removed from a surface seen through a
                                 //     nearer transparent one; <= 0 inks every surface
} params;

layout(location = 0) out vec4 out_color;

// 8x8 Bayer ordered-dither matrix, values 0..63.
const float BAYER8[64] = float[64](
     0,32, 8,40, 2,34,10,42,
    48,16,56,24,50,18,58,26,
    12,44, 4,36,14,46, 6,38,
    60,28,52,20,62,30,54,22,
     3,35,11,43, 1,33, 9,41,
    51,19,59,27,49,17,57,25,
    15,47, 7,39,13,45, 5,37,
    63,31,55,23,61,29,53,21
);

// True view-space distance from the camera, from raw Vulkan [0,1]
// post-projection depth. Perspective depth is hyperbolic (glm::perspective ->
// perspectiveRH_ZO); orthographic depth is already linear in the raw value
// (glm::ortho -> orthoRH_ZO), hence the branch.
float linear_depth(float d) {
    if (params.camera_is_perspective < 0.5) {
        return mix(params.camera_near, params.camera_far, d);
    }
    return params.camera_near * params.camera_far /
           (params.camera_far - d * (params.camera_far - params.camera_near));
}

bool is_background(vec3 n) { return dot(n, n) < 0.001; }

// Does the surface under `uv` want an outline? True for everything in `all` mode. In `toon`
// mode, only where the material set its outline bit.
//
// Tested on the CENTRE texel alone, not its neighbours: edge_ink_at() below is deliberately
// one-sided (see its inverse-depth comment -- it fires on the texel that pops TOWARD the
// camera, not on the surface behind it), so the texel being asked about is always the nearer
// surface's own. That makes "outline this material" mean its silhouette against the sky, its
// silhouette against anything behind it, and its own interior creases -- and leaves a PBR
// object in FRONT of an outlined one un-lined, which is what per-material opt-in should do.
bool wants_outline(vec2 uv) {
    if (params.outline_mode < 0.5) return true;
    uint code = uint(max(texture(outline_mask, uv).a, 0.0) + 0.5);
    return (code & (1u << 10)) != 0u;
}

/**
 * True where the material at `uv` is toon-BANDED -- bits 0-2 of the same per-material code
 * wants_outline() reads, where 0 means PBR shading.
 *
 * Banding is the right question to ask on behalf of the hatch (rather than, say, "is this
 * the terrain"): a banded surface presents the hatch with flat steps to sit on, which is
 * what makes a stroke read as drawn shading instead of as texture over a gradient. A
 * material opts in by asking to be toon-shaded, and the stylize pass never learns what kind
 * of object it is looking at.
 */
bool is_toon_banded(vec2 uv) {
    uint code = uint(max(texture(outline_mask, uv).a, 0.0) + 0.5);
    return (code & 7u) != 0u;
}

/**
 * Ink multiplier at `uv` for the "seen through water" rule: 1.0 normally, and
 * `1 - outline_through_water` where a transparent surface covers this pixel.
 *
 * Coverage being non-zero is the WHOLE test. Transparents are depth-TESTED against the
 * opaque depth (TransparentPass's file doc) even though they never depth-write, so a
 * fragment only reaches the attachment if it already passed in front of whatever opaque
 * surface this pixel holds. "A transparent surface wrote here" therefore already means "it
 * is in front".
 *
 * Do NOT add a depth comparison to double-check that. The attachment stores the reciprocal
 * of RADIAL camera distance (`1/length(world_pos - camera_pos)`, transparent_fs.glsl),
 * while linear_depth() above returns view-space Z along the camera axis. The two agree only
 * on the view axis and diverge as `1/cos(theta)` off it -- ~30% at the horizontal edge of a
 * 50-degree fov 16:9 frame, against a water-to-bed separation that is under 5% of depth. A
 * compare between them holds at the centre of the screen and fails toward its edges, which
 * reads as an outline that switches back on only when the object is panned off-centre.
 */
float through_water_ink(vec2 uv) {
    if (params.outline_through_water <= 0.0) return 1.0;
    if (texture(transparent_coverage, uv).r <= 0.0) return 1.0;  // nothing transparent here
    return 1.0 - clamp(params.outline_through_water, 0.0, 1.0);
}

/**
 * Ink at `uv` for one ring at radius `texel`: 1.0 on a SILHOUETTE, `crease` on an interior
 * CREASE, 0.0 on neither.
 *
 * The two kinds are separated because only one of them may fade with distance. A silhouette
 * ("this surface ends here" -- a background neighbour, or a genuine step in depth) is what
 * makes a subject readable at all. A crease ("this surface folds here") is interior detail,
 * and a subject small enough has a fold under very nearly every texel, so inking them all
 * fills it in solid -- see crease_strength() below.
 *
 * One function rather than two so the four normal taps are shared: the crease test needs
 * exactly the neighbourhood the silhouette test already fetched.
 */
float edge_ink_at(vec2 uv, vec3 n0, vec2 texel, float crease) {
    vec2 dx = vec2(texel.x, 0.0);
    vec2 dy = vec2(0.0, texel.y);

    vec3 nx  = texture(scene_normal, uv + dx).rgb;
    vec3 ny  = texture(scene_normal, uv + dy).rgb;
    vec3 nxn = texture(scene_normal, uv - dx).rgb;
    vec3 nyn = texture(scene_normal, uv - dy).rgb;

    // A background neighbour is an unconditional silhouette edge: there is no
    // depth or normal on that side to compare against, so if it were reachable
    // by the depth test at all, the sky sits at the far plane where hyperbolic
    // depth precision is at its worst.
    if (is_background(nx) || is_background(ny) || is_background(nxn) || is_background(nyn)) {
        return 1.0;
    }

    float z0  = linear_depth(texture(scene_depth, uv).r);
    float zx  = linear_depth(texture(scene_depth, uv + dx).r);
    float zy  = linear_depth(texture(scene_depth, uv + dy).r);
    float zxn = linear_depth(texture(scene_depth, uv - dx).r);
    float zyn = linear_depth(texture(scene_depth, uv - dy).r);

    // 1/z is affine in screen space over any plane, however steeply it
    // recedes -- so on a plane the centre sits exactly halfway between its
    // two opposite neighbours in inverse depth, and this is identically
    // zero. Only a genuine step in Z (not just a grazing viewing angle)
    // makes it non-zero. The *z0 turns the raw 1/z gap (~Delta z / z^2) into
    // a relative step (~Delta z / z), so one depth_threshold works at any
    // distance. The signed (not abs) form keeps the line one-sided: it
    // fires on the texel that pops toward the camera, not on the surface
    // behind it.
    float rel_x = (1.0 / z0 - 0.5 * (1.0 / zx + 1.0 / zxn)) * z0;
    float rel_y = (1.0 / z0 - 0.5 * (1.0 / zy + 1.0 / zyn)) * z0;
    if (max(rel_x, rel_y) > params.depth_threshold) return 1.0;

    if (crease <= 0.0) return 0.0;
    float min_dot = min(min(dot(n0, normalize(nx)), dot(n0, normalize(ny))),
                         min(dot(n0, normalize(nxn)), dot(n0, normalize(nyn))));
    return (min_dot < params.normal_threshold) ? crease : 0.0;
}

/**
 * How strongly interior creases ink at `uv`, 0..1. 1 everywhere when the fade is disabled.
 *
 * A pen keeps ONE line weight and draws LESS as a subject recedes -- it does not thin its
 * nib. What has to go first is interior detail: a figure fifteen pixels tall has a normal
 * fold under very nearly every texel, so inking them all fills the figure in solid, which is
 * the "zoomed-out character becomes a black smudge" failure. Silhouettes are never faded --
 * losing those would dissolve the subject instead of simplifying it.
 *
 * Keyed on how much WORLD a render texel spans at this fragment (z / outline_focal_px under
 * perspective; constant under orthographic) rather than on raw view distance, so one authored
 * value survives a change of fov, of render resolution, or of the scale a scene is built at --
 * the same reasoning as gfx/ssr_common.glsl's ssr_texel_world_size().
 */
float crease_strength(vec2 uv) {
    if (params.outline_crease_fade <= 0.0 || params.outline_focal_px <= 0.0) return 1.0;
    float texel_world = 1.0 / params.outline_focal_px;
    if (params.camera_is_perspective >= 0.5) {
        texel_world *= max(linear_depth(texture(scene_depth, uv).r), 1.0e-4);
    }
    // Full detail out to half the configured span, gone by the span itself.
    return 1.0 - smoothstep(params.outline_crease_fade * 0.5, params.outline_crease_fade,
                             texel_world);
}

/**
 * Ink coverage at `uv`, 0..1, over a line `steps` render texels wide.
 *
 * A silhouette always inks fully; a crease inks at `crease` (see crease_strength). The line
 * keeps ONE weight -- `steps` whole texels -- at every distance, because `crease` controls how
 * much is drawn rather than how thick it is drawn.
 */
float outline_coverage(vec2 uv, vec3 n0, int steps, float crease) {
    float ink = 0.0;
    for (int i = 1; i <= steps; ++i) {
        float e = edge_ink_at(uv, n0, float(i) * params.inv_render_size, crease);
        if (e >= 1.0) return 1.0;       // a silhouette; nothing can ink harder
        ink = max(ink, e);
    }
    return ink;
}

// ACES fitted curve (Narkowicz).
vec3 aces_film(vec3 x) {
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Exact inverse of upscale.frag's srgb_decode() -- see this pass's main() for why it runs
// here, right after the tonemap curve.
vec3 srgb_encode(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 quantize_to_palette(vec3 color) {
    int count = int(params.palette_count);
    if (count <= 0) return color;

    vec3 best = color;
    float best_dist = 1.0 / 0.0; // +inf
    for (int i = 0; i < count; ++i) {
        float u = (float(i) + 0.5) / params.palette_count;
        vec3 entry = texture(palette_lut, vec2(u, 0.5)).rgb;
        vec3 diff = entry - color;
        float dist = dot(diff, diff);
        if (dist < best_dist) {
            best_dist = dist;
            best = entry;
        }
    }
    return best;
}

// Angle offsets, in degrees, of hatch layers 1 and 2 relative to hatch_angle_deg.
// Deliberately NOT 90/45: a second pass ruled exactly square on the first beats against
// the pixel grid into a visible plaid, and a hand never lands square anyway. 87/41 read
// as a cross and are far enough off to kill the beat.
const float HATCH_LAYER_ANGLE[3] = float[3](0.0, 87.0, 41.0);

// Share of one stroke period a fully-engaged layer inks. Below 1 on purpose: a layer
// that closes up has no strokes left to read AS strokes. Three layers at 0.5 already
// reach 1 - 0.5^3 = 88% coverage in the deepest shadow, which is as black as ink gets.
const float HATCH_MAX_DUTY = 0.5;

// Triplanar weight below which an axis is skipped entirely (surface space). With the
// weights raised to HATCH_TRIPLANAR_SHARPNESS, at most two axes ever clear this, so the
// loop costs two stroke evaluations and not three -- except exactly on the (1,1,1)
// diagonal, where three strokes is the correct answer anyway.
const float HATCH_AXIS_CUTOFF = 0.02;
const float HATCH_TRIPLANAR_SHARPNESS = 16.0;

/**
 * Ink coverage of one ruled stroke set at this fragment, 0..1.
 *
 * @param p      Position in the stroke plane, ALREADY divided by the stroke period --
 *               so one unit of p is one period, whatever space the caller is working in.
 * @param angle_deg Stroke direction in that plane.
 * @param duty   Share of the period this set inks. 0 is an exact no-op.
 * @param aa     |d dist / d screen pixel|, which the caller knows analytically.
 *
 * Why `aa` is a parameter rather than an fwidth() in here: `dist` below is built from a
 * fract(), so its screen derivative is a spike at every stroke boundary and fwidth()
 * there returns ~1 -- a one-pixel white seam down the middle of every gap. The true
 * gradient is constant within a stroke set and the caller has it in closed form. The
 * side benefit is that this function takes no derivative at all, so it is safe under
 * the non-uniform control flow the triplanar axis cutoff and the sky fallback introduce
 * (contrast gfx/toon.glsl's gfx_toon_ramp(), which had to clamp around exactly that).
 */
float hatch_stroke(vec2 p, float angle_deg, float duty, float aa, float softness) {
    if (duty <= 0.0) return 0.0;
    float a = radians(angle_deg);
    // Project onto the stroke NORMAL -- the direction the strokes repeat along.
    float s = dot(p, vec2(-sin(a), cos(a)));
    float u = fract(s);
    // Distance from the nearest stroke centreline: 0 on the line, 1 midway between two.
    // Scaled so the share of the period with dist < d is exactly d, which is what lets
    // `duty` be read straight off as a coverage.
    float dist = min(u, 1.0 - u) * 2.0;

    float w = clamp(aa * (1.0 + softness * 8.0), 1.0e-4, 0.5);
    // The second factor does two jobs. It makes duty == 0 an EXACT zero (a plain
    // smoothstep(duty-w, duty+w, dist) returns 0.5 on the centreline at duty 0, i.e.
    // a half-ink line at zero tone), and it antialiases a stroke thinner than one
    // pixel by fading its opacity instead of its width -- the correct coverage
    // integral, and what stops the thinnest strokes popping in and shimmering as the
    // tone crosses a layer's engagement point.
    return (1.0 - smoothstep(duty - w, duty + w, dist)) * smoothstep(0.0, w, duty);
}

/// Screen-locked stroke sets. A FIXED screen frequency: it neither aliases as a surface
/// recedes nor changes when the camera moves, so a hatched frame stays inside this
/// engine's "a static camera renders a static image" contract
/// (temporal_stability_test.cpp's image_settles_after_camera_stops). The cost is that the
/// strokes belong to the paper rather than to the object -- which, for a pen look, is the
/// right owner.
float hatch_screen(float darkness, int n, float period, float softness, float angle0) {
    vec2  p  = gl_FragCoord.xy / period;
    float aa = 2.0 / period;
    float ink = 0.0;
    for (int i = 0; i < n; ++i) {
        float fill = clamp((darkness - float(i) / float(n)) * float(n), 0.0, 1.0);
        ink = max(ink, hatch_stroke(p, angle0 + HATCH_LAYER_ANGLE[i],
                                    fill * HATCH_MAX_DUTY, aa, softness));
    }
    return ink;
}

/**
 * World-locked stroke sets: strokes stick to the surface, so a wall carries the same
 * strokes as the camera moves past it.
 *
 * Triplanar, selected by the world normal already bound at binding 2, so this needs no
 * UVs, no tangent frame and no view/projection matrix -- which is what keeps it inside
 * the push-constant budget.
 *
 * ## Frequency
 * A fixed world period aliases into mush at distance. So the period is picked per
 * fragment to project to roughly `period_px` screen pixels: `wpx` world units per screen
 * pixel comes from fwidth() of the world position, and the period is quantized to a
 * power of two and CROSSFADED with the next octave by frac(log2) -- the same mip-blend a
 * tonal-art-map hatch does with textures, done analytically. Without the crossfade there
 * is a visible band across the image wherever the octave changes.
 *
 * ## Axis blending
 * The weights are raised to HATCH_TRIPLANAR_SHARPNESS and the ink values -- not the
 * coordinates -- are blended. Blending coordinates would smear two stroke sets into a
 * plaid over a broad transition; blending coverage under a sharpened weight leaves a
 * band a few degrees wide where both sets show at partial opacity, which reads as a pen
 * changing direction at a form break. There is no continuous alternative: a globally
 * continuous stroke direction on a closed surface does not exist (hairy ball), so the
 * only question is where to put the discontinuity, and the triplanar ridge -- which
 * coincides with a form edge -- is the best available place.
 *
 * ## KNOWN LIMITATION: large world coordinates
 * `wp` comes from the G-buffer's G2 attachment, RGBA16F. Its quantization step at world
 * coordinates of a few hundred units is already comparable to a stroke period (the same
 * reason SSAO reconstructs its shading point from depth instead of reading G2 at all).
 * The LOD above mitigates this while the CAMERA is near the origin (period and lattice
 * step both grow roughly linearly with distance, so their ratio holds out to a few
 * hundred units) and does nothing when the camera itself sits at a large coordinate: on
 * a streamed open world, strokes snap onto the half-float lattice and shift under camera
 * motion. Hence hatch_space defaults to screen, and surface is for scenes composed near
 * the origin.
 */
float hatch_surface(float darkness, int n, float period_px, float softness, float angle0,
                    vec3 wp, float wpx, vec3 nrm) {
    vec3 an = abs(nrm);
    vec3 wt = pow(an, vec3(HATCH_TRIPLANAR_SHARPNESS));
    wt /= max(wt.x + wt.y + wt.z, 1.0e-6);

    // Power-of-two world period bracketing the one that projects to period_px pixels.
    float l  = log2(max(period_px * wpx, 1.0e-5));
    float l0 = floor(l);
    float f  = l - l0;                 // crossfade weight between the two octaves
    float P0 = exp2(l0), P1 = P0 * 2.0;
    float aa0 = 2.0 * wpx / P0, aa1 = 2.0 * wpx / P1;

    float ink = 0.0;
    for (int ax = 0; ax < 3; ++ax) {
        if (wt[ax] < HATCH_AXIS_CUTOFF) continue;
        // The two world axes spanning the plane this face projects onto.
        vec2 q = ax == 0 ? wp.yz : (ax == 1 ? wp.zx : wp.xy);
        float axis_ink = 0.0;
        for (int i = 0; i < n; ++i) {
            float fill = clamp((darkness - float(i) / float(n)) * float(n), 0.0, 1.0);
            float duty = fill * HATCH_MAX_DUTY;
            float ang  = angle0 + HATCH_LAYER_ANGLE[i];
            axis_ink = max(axis_ink,
                           mix(hatch_stroke(q / P0, ang, duty, aa0, softness),
                               hatch_stroke(q / P1, ang, duty, aa1, softness), f));
        }
        ink += wt[ax] * axis_ink;      // sum, not max: the weights sum to 1, so a
    }                                  // single-axis region yields exactly axis_ink
    return clamp(ink, 0.0, 1.0);
}

/**
 * Total ink coverage, keyed off the fragment's own tone.
 *
 * `luma` is DISPLAY-REFERRED: this runs after the tonemap, the sRGB encode and the
 * grading LUT, so the key is the brightness the viewer actually sees and
 * hatch_threshold can be read straight off a screenshot. Darkness is split EVENLY
 * between the layers: layer i engages once darkness passes i/n and reaches full duty at
 * (i+1)/n. So tone is carried continuously -- by growing stroke width inside a layer and
 * by a new crossing layer between them -- rather than by n discrete steps.
 *
 * `threshold` is passed in rather than read from `params` because it is per-PIXEL: a
 * non-toon-banded surface uses hatch_pbr_threshold instead, so that a smoothly-lit ground
 * is not hatched end to end by a cutoff tuned for a banded subject. See main().
 */
float hatch_coverage(float luma, vec3 wp, float wpx, vec3 nrm, bool surface, float threshold) {
    int   n = clamp(int(round(params.hatch_layers)), 1, 3);
    float t = max(threshold, 1.0e-4);
    float darkness = clamp((t - luma) / t, 0.0, 1.0);
    float period   = max(params.hatch_scale, 2.0);
    if (surface) {
        return hatch_surface(darkness, n, period, params.hatch_softness,
                             params.hatch_angle_deg, wp, wpx, nrm);
    }
    return hatch_screen(darkness, n, period, params.hatch_softness, params.hatch_angle_deg);
}

/**
 * Screen-space displacement of the ink contour, in texels.
 *
 * A static, low-frequency field: the outline detector is evaluated at a slightly wrong
 * place, which makes a mathematically exact depth/normal edge read as a stroke laid
 * down by a hand that was not quite following it. Static (gl_FragCoord and nothing
 * else), so a still frame is still. The consequence is that the wobble belongs to the
 * SCREEN: a contour sliding across the frame under camera motion crawls through the
 * field. That is the "boil" of hand-inked animation, and it is why this defaults to 0.
 *
 * ~22 px features -- well above the outline's own 1-2 texel reach, so the detector sees
 * a coherently displaced neighbourhood rather than per-pixel hash.
 */
vec2 ink_wobble() {
    if (params.outline_wobble <= 0.0) return vec2(0.0);
    vec3 p = vec3(gl_FragCoord.xy / 22.0, 0.0);
    vec2 n = vec2(gfx_value_noise_3d(p), gfx_value_noise_3d(p + vec3(11.3, 7.7, 0.0)));
    return (n * 2.0 - 1.0) * params.outline_wobble * params.inv_render_size;
}

/**
 * Multiplier on INK COVERAGE from the paper's tooth, centred on 1.0. Ink skips where a
 * fibre stands proud and pools in the hollow beside it, so a stroke's edge breaks up the
 * way a nib's does. Returns exactly 1.0 when disabled, and bare paper the pen never
 * touched is never perturbed by it at all -- unlike adding this to the final colour,
 * which would perturb every pixel on its way into the palette quantizer regardless of
 * whether anything inked it. Two scales: one render texel, and a 3-texel fibre clump.
 */
float paper_tooth() {
    if (params.paper_strength <= 0.0) return 1.0;
    float fine  = gfx_hash13(vec3(gl_FragCoord.xy, 0.0));
    float fibre = gfx_hash13(vec3(floor(gl_FragCoord.xy / 3.0), 7.0));
    return 1.0 + (mix(fine, fibre, 0.5) - 0.5) * params.paper_strength;
}

void main() {
    vec3 color = texture(scene_color, in_uv).rgb;

    // Bloom: additive soft glow in linear HDR, before the tonemap, since it is a light
    // contribution rather than a stylistic filter over the final image. Thresholding already
    // happened once in bloom_prefilter.frag, per source texel and before any blurring --
    // deliberately NOT repeated here: subtracting a threshold from an already-blurred image
    // would eat the halo falloff the pyramid exists to produce, and would put a hard
    // C0 cutoff back on a value that drifts under camera motion (the shimmer this pass used
    // to have when bloom was a single reused mip lookup).
    if (params.bloom_intensity > 0.0) {
        color += texture(bloom_tex, in_uv).rgb * params.bloom_intensity;
    }

    // Tonemap, then encode to sRGB so the 8-bit UNORM write below quantizes in perceptual
    // space instead of linear -- linear quantization wastes almost all 256 codes on the top
    // half of the range and leaves darks (exactly where a bloom falloff or a sky gradient
    // lives) with a handful of codes each, which is what was producing visible banding
    // through this pass's low-slope regions. Everything from here down (outline_color, the
    // dither, palette_lut) is authored/stored display-referred, so this is also the one place
    // in the pass where the switch has to happen. Gated on the same exposure>0 branch as the
    // tonemap itself: a consumer with its own tonemap chain (exposure<=0, see this file's
    // StylizePass doc) feeds already-tonemapped, already-encoded LDR and must not have
    // this reapplied.
    if (params.exposure > 0.0) {
        // Auto-exposure multiplies the configured exposure rather than replacing it,
        // so the config value keeps its meaning as the scene's baseline stop and the
        // metered term is a relative adaptation on top (HDRP's own split between
        // Fixed exposure and Exposure Compensation).
        float exposure = params.exposure;
        if (params.auto_exposure != 0.0) {
            exposure *= texture(exposure_tex, vec2(0.5)).r;
        }
        color = aces_film(color * exposure);
        color = srgb_encode(color);
    }

    // Colour grading, on DISPLAY-REFERRED colour: after the tonemap and sRGB encode
    // (a LUT authored in an image editor expects the values it would see there) and
    // before outline/dither/palette, which are stylization the grade must not touch --
    // an outline is a fixed authored colour, and quantizing to a palette after grading
    // would grade the palette entries themselves off-palette.
    color = gfx_apply_grading_lut(grading_lut, color, params.grading_size);

    // Crosshatch: keyed off the GRADED tone, and UNDER the ink contour below -- the
    // order a pen works in, shading strokes first and the contour over them.
    // Deliberately NOT gated on a surface being there: a Moebius sky is hatched as
    // readily as a rock, and the sky's own tone is what decides. In surface space a
    // background pixel has no world position, so it falls back to the screen-space
    // field rather than going blank, which would make a dark sky pop against hatched
    // ground.
    //
    // Inks with outline_color.rgb -- see this file's header for why that coupling
    // is deliberate.
    float tooth = paper_tooth();
    if (params.hatch_strength > 0.0) {
        // Hoisted out of every per-pixel branch, under a push-constant (and therefore
        // uniform) condition, so the fwidth() below has a defined value. It still spikes
        // on a quad straddling a silhouette, where neighbouring world positions genuinely
        // differ -- hence the clamp, and the one wrong-LOD quad that leaves sits exactly
        // where the ink line is drawn anyway.
        vec3  wp = vec3(0.0);
        float wpx = 1.0;
        vec3  nrm = texture(scene_normal, in_uv).rgb;
        bool  surface = false;
        if (params.hatch_space >= 0.5) {
            wp  = texture(g_world_pos, in_uv).rgb;
            wpx = clamp(length(fwidth(wp)), 1.0e-5, 1.0e3);
            surface = !is_background(nrm);
            if (surface) nrm = normalize(nrm);
        }
        float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
        // A surface whose material is not toon-banded keys its hatch off its own threshold,
        // so a smoothly-lit ground keeps ink only where it is genuinely dark (a shadow cast
        // onto it) while a banded subject still hatches across its mid-tone. Gated on
        // `surface` as well: the background is not a material, and a Moebius sky is hatched
        // as readily as a rock -- see the comment at this block's head.
        float threshold = params.hatch_threshold;
        if (params.hatch_pbr_threshold > 0.0 && !is_background(texture(scene_normal, in_uv).rgb) &&
            !is_toon_banded(in_uv)) {
            threshold = params.hatch_pbr_threshold;
        }
        float ink  = hatch_coverage(luma, wp, wpx, nrm, surface, threshold) *
                     clamp(params.hatch_strength, 0.0, 1.0) * tooth;
        color = mix(color, params.outline_color.rgb, clamp(ink, 0.0, 1.0));
    }

    // The ink contour. EVERYTHING the detector reads is sampled at ink_uv, not in_uv:
    // displacing the whole evaluation (normal, per-material gate and edge test alike)
    // moves the LINE, where displacing only the edge test would leave the silhouette
    // gate at in_uv and clip every outward wobble against it -- a nibbled edge rather
    // than a wobbling one. outline_wobble 0 makes ink_uv bit-identical to in_uv.
    vec2 ink_uv = in_uv + ink_wobble();
    vec3 n0 = texture(scene_normal, ink_uv).rgb;
    int ink_steps = max(int(round(params.outline_thickness)), 0);
    float cover = 0.0;
    if (ink_steps > 0 && !is_background(n0) && wants_outline(ink_uv)) {
        cover = outline_coverage(ink_uv, normalize(n0), ink_steps, crease_strength(ink_uv)) *
                tooth * through_water_ink(ink_uv);
    }
    if (cover > 0.0) {
        color = mix(color, params.outline_color.rgb, params.outline_color.a * clamp(cover, 0.0, 1.0));
    }

    if (params.dither_strength > 0.0) {
        ivec2 texel = ivec2(gl_FragCoord.xy);
        float t = (BAYER8[(texel.y & 7) * 8 + (texel.x & 7)] / 63.0 - 0.5) * params.dither_strength;
        color += vec3(t);
    }

    color = quantize_to_palette(clamp(color, 0.0, 1.0));

    out_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
