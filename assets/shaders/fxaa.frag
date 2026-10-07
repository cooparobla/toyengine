#version 450

// Standalone LDR FXAA 3.11 Quality Preset 39 (Ultra Quality - 12 steps along edge).
//
// The same algorithm as gfxcoopa ToneMappingPass's fused FXAA+tonemap path (used by blendy)
// -- same QUALITY_STEP table, same early-out, same edge classification / bidirectional
// search / pixelOffset / correctVariation / smoothstep^2 subpixel term -- but FetchLdr()
// reads an already-tonemapped LDR source (the caller decides when/whether to tonemap
// upstream) rather than re-tonemapping HDR on every tap. FetchLdr() uses a *sqrt* perceptual
// luma -- not linear luma -- so edge detection sees the same contrast either way.
layout(location = 0) in vec2 in_uv;

layout(set = 0, binding = 0) uniform sampler2D color_sampler;

layout(push_constant) uniform FxaaPush {
    float screen_width;
    float screen_height;
    float subpixel_quality;
    float edge_threshold;
    float edge_threshold_min;
} push;

layout(location = 0) out vec4 out_color;

// Convert linear RGB color to perceptual luma
float RGB2Luma(vec3 rgb) {
    return sqrt(dot(rgb, vec3(0.299, 0.587, 0.114)));
}

// Sample the LDR source at UV, stashing perceptual luma in .a
vec4 FetchLdr(vec2 uv) {
    vec3 c = texture(color_sampler, uv).rgb;
    return vec4(c, RGB2Luma(c));
}

#define FXAA_QUALITY_STEPS 12
const float QUALITY_STEP[12] = float[12](
    1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0
);

vec3 FxaaPixelShader(vec2 uv, vec2 inverse_screen_size) {
    // 1. Center sample
    vec4 centerSample = FetchLdr(uv);
    vec3 colorCenter = centerSample.rgb;
    float lumaCenter = centerSample.a;

    // 2. Cardinal neighbor luma samples
    float lumaDown  = FetchLdr(uv + vec2(0.0, -1.0) * inverse_screen_size).a;
    float lumaUp    = FetchLdr(uv + vec2(0.0,  1.0) * inverse_screen_size).a;
    float lumaLeft  = FetchLdr(uv + vec2(-1.0, 0.0) * inverse_screen_size).a;
    float lumaRight = FetchLdr(uv + vec2( 1.0, 0.0) * inverse_screen_size).a;

    // Find min and max luma in cardinal neighborhood
    float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
    float lumaRange = lumaMax - lumaMin;

    // Early exit if contrast range is below threshold (no anti-aliasing needed)
    float threshold = max(push.edge_threshold_min, lumaMax * push.edge_threshold);
    if (lumaRange < threshold) {
        return colorCenter;
    }

    // 3. Diagonal neighbor luma samples for subpixel and edge direction calculation
    float lumaDownLeft  = FetchLdr(uv + vec2(-1.0, -1.0) * inverse_screen_size).a;
    float lumaUpRight   = FetchLdr(uv + vec2( 1.0,  1.0) * inverse_screen_size).a;
    float lumaUpLeft    = FetchLdr(uv + vec2(-1.0,  1.0) * inverse_screen_size).a;
    float lumaDownRight = FetchLdr(uv + vec2( 1.0, -1.0) * inverse_screen_size).a;

    // Combine cardinal and diagonal lumas for box filter
    float lumaDownUp    = lumaDown + lumaUp;
    float lumaLeftRight = lumaLeft + lumaRight;
    float lumaLeftCorners  = lumaDownLeft + lumaUpLeft;
    float lumaRightCorners = lumaDownRight + lumaUpRight;
    float lumaUpCorners    = lumaUpLeft + lumaUpRight;
    float lumaDownCorners  = lumaDownLeft + lumaDownRight;

    // 4. Calculate edge orientation (Horizontal vs. Vertical)
    float edgeHorizontal = abs(-2.0 * lumaLeft  + lumaLeftCorners)  +
                           abs(-4.0 * lumaCenter + 2.0 * lumaDownUp)  +
                           abs(-2.0 * lumaRight + lumaRightCorners);

    float edgeVertical   = abs(-2.0 * lumaUp    + lumaUpCorners)    +
                           abs(-4.0 * lumaCenter + 2.0 * lumaLeftRight) +
                           abs(-2.0 * lumaDown  + lumaDownCorners);

    bool isHorizontal = (edgeHorizontal >= edgeVertical);

    // 5. Select edge normal direction (-1 or +1)
    float luma1 = isHorizontal ? lumaDown : lumaLeft;
    float luma2 = isHorizontal ? lumaUp   : lumaRight;

    float gradient1 = abs(luma1 - lumaCenter);
    float gradient2 = abs(luma2 - lumaCenter);

    bool is1Steeper = (gradient1 >= gradient2);
    float gradientScaled = 0.25 * max(gradient1, gradient2);

    // Step offset perpendicular to edge
    float stepLength = isHorizontal ? inverse_screen_size.y : inverse_screen_size.x;
    float lumaOpposite = is1Steeper ? luma1 : luma2;
    if (is1Steeper) {
        stepLength = -stepLength;
    }

    // Move UV to edge boundary (half-pixel step into normal)
    vec2 currentUv = uv;
    if (isHorizontal) {
        currentUv.y += stepLength * 0.5;
    } else {
        currentUv.x += stepLength * 0.5;
    }

    // 6. Tangent step direction along edge
    vec2 offset = isHorizontal ? vec2(inverse_screen_size.x, 0.0) : vec2(0.0, inverse_screen_size.y);

    vec2 uvN = currentUv - offset;
    vec2 uvP = currentUv + offset;

    float lumaAverage = 0.5 * (lumaCenter + lumaOpposite);

    float lumaEndN = FetchLdr(uvN).a - lumaAverage;
    float lumaEndP = FetchLdr(uvP).a - lumaAverage;

    bool reachedN = abs(lumaEndN) >= gradientScaled;
    bool reachedP = abs(lumaEndP) >= gradientScaled;
    bool reachedBoth = reachedN && reachedP;

    if (!reachedN) uvN -= offset;
    if (!reachedP) uvP += offset;

    // 7. Iterative search along edge tangent (12 steps, Quality 39)
    if (!reachedBoth) {
        for (int i = 2; i < FXAA_QUALITY_STEPS; i++) {
            if (!reachedN) {
                lumaEndN = FetchLdr(uvN).a - lumaAverage;
                reachedN = abs(lumaEndN) >= gradientScaled;
            }
            if (!reachedP) {
                lumaEndP = FetchLdr(uvP).a - lumaAverage;
                reachedP = abs(lumaEndP) >= gradientScaled;
            }
            reachedBoth = reachedN && reachedP;

            if (!reachedN) uvN -= offset * QUALITY_STEP[i];
            if (!reachedP) uvP += offset * QUALITY_STEP[i];

            if (reachedBoth) break;
        }
    }

    // 8. Distance calculation to edge ends
    float distN = isHorizontal ? (uv.x - uvN.x) : (uv.y - uvN.y);
    float distP = isHorizontal ? (uvP.x - uv.x) : (uvP.y - uv.y);
    if (distN < 0.0) distN = -distN;
    if (distP < 0.0) distP = -distP;

    bool isDirectionN = distN < distP;
    float distMin = min(distN, distP);
    float distSpan = distN + distP;

    float pixelOffset = -distMin / distSpan + 0.5;

    // Polarity check: does the luma sign match center to edge endpoint?
    float lumaEndMin = isDirectionN ? lumaEndN : lumaEndP;
    bool correctVariation = (lumaEndMin < 0.0) != (lumaCenter - lumaAverage < 0.0);

    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // 9. Sub-pixel antialiasing blending calculation
    float lumaL = (lumaDownUp + lumaLeftRight) * 0.25;
    float subpixelOffset1 = clamp(abs(lumaL - lumaCenter) / lumaRange, 0.0, 1.0);
    float subpixelOffset2 = (-2.0 * subpixelOffset1 + 3.0) * subpixelOffset1 * subpixelOffset1; // smoothstep
    float subpixelOffsetFinal = subpixelOffset2 * subpixelOffset2 * push.subpixel_quality;

    finalOffset = max(finalOffset, subpixelOffsetFinal);

    // 10. Sample final color at calculated offset UV coordinate
    vec2 finalUv = uv;
    if (isHorizontal) {
        finalUv.y += finalOffset * stepLength;
    } else {
        finalUv.x += finalOffset * stepLength;
    }

    return FetchLdr(finalUv).rgb;
}

void main() {
    vec2 invScreen = vec2(1.0 / push.screen_width, 1.0 / push.screen_height);
    out_color = vec4(FxaaPixelShader(in_uv, invScreen), 1.0);
}
