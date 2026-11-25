#ifndef TOY_SKY_PHYSICAL_GLSL
#define TOY_SKY_PHYSICAL_GLSL

// sky_physical.glsl -- the physical sky as the lighting pass draws it at background pixels
// (render sky_model: physical): the sky-view LUT, the stars, the sun's and the moon's discs. Also
// the half-resolution volumetric cloud layer's upsample (sky_with_clouds), which composites it
// over either sky model.
//
// REQUIRED BEFORE INCLUDE: the `camera` block (camera_pos), the `lights` block
// (light_ubo_body.glsl: sky_sun / sky_moon / sky_params / sky_extra), and the samplers
// u_sky_transmittance, u_sky_view and u_sky_clouds.
//
// OPTIONAL: #define TOY_SKY_BODY_ATLAS before include, in a pass that also binds the
// u_sky_bodies sampler (the sky-body atlas) -- an additional moon disc then samples its face
// from the atlas. Without the define the disc falls back to the procedural maria, so passes
// without the binding (the cloud resolve and composite) still draw every disc.

#include "sky_atmosphere.glsl"
#include "sky_cloud_common.glsl"

float sky_hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
vec3 sky_hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

float sky_vnoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n00 = mix(sky_hash13(i), sky_hash13(i + vec3(1, 0, 0)), f.x);
    float n10 = mix(sky_hash13(i + vec3(0, 1, 0)), sky_hash13(i + vec3(1, 1, 0)), f.x);
    float n01 = mix(sky_hash13(i + vec3(0, 0, 1)), sky_hash13(i + vec3(1, 0, 1)), f.x);
    float n11 = mix(sky_hash13(i + vec3(0, 1, 1)), sky_hash13(i + vec3(1, 1, 1)), f.x);
    return mix(mix(n00, n10, f.y), mix(n01, n11, f.y), f.z);
}

// Smooth blotches over the moon's face (its darker maria), in [0, 1].
float sky_moon_maria(vec2 p) {
    float v = 0.0, a = 0.5;
    p = p * 2.2 + 7.0;
    for (int o = 0; o < 4; ++o) {
        vec2 i = floor(p), f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        float n = mix(mix(sky_hash13(vec3(i, 1.0)), sky_hash13(vec3(i + vec2(1, 0), 1.0)), f.x),
                      mix(sky_hash13(vec3(i + vec2(0, 1), 1.0)), sky_hash13(vec3(i + vec2(1, 1), 1.0)), f.x), f.y);
        v += a * n;
        p *= 2.1;
        a *= 0.5;
    }
    return smoothstep(0.45, 0.75, v);
}

// One tinted, limb-darkened sun disc with an anti-aliased rim and a tight corona hugging it;
// black past the corona. The rim's anti-aliasing is one-to-two PIXELS wide at any apparent size
// (fwidth, the same reasoning as sky_moon_disc's) -- a fixed fraction of the radius was a
// hundred-pixel halo on a sun filling half the sky, which read as a formless blob.
vec3 sky_sun_disc(vec3 dir, vec4 sun, vec3 tint, float radiance) {
    float cs = dot(dir, sun.xyz);
    float rad = max(acos(clamp(sun.w, -1.0, 1.0)), 1e-5);
    float r = acos(clamp(cs, -1.0, 1.0)) / rad;   // 0 centre .. 1 rim
    float px = clamp(fwidth(r), 1e-4, 0.5);
    if (radiance <= 0.0 || r >= 3.0) return vec3(0.0);
    float edge = 1.0 - smoothstep(1.0 - px, 1.0 + px, r);
    float mu = sqrt(max(1.0 - min(r, 1.0) * min(r, 1.0), 0.0));
    float limb = 1.0 - 0.6 * (1.0 - pow(mu, 0.8));
    // The corona: a thin glow falling off from the rim, so a disc whose radiance the app tamed
    // for its size still reads as a glowing star rather than a hard-cut circle.
    float corona = exp(-max(r - 1.0, 0.0) * 5.0) * (1.0 - edge) * 0.10;
    return tint * (radiance * (edge * limb + corona));
}

// One moon disc, shaded as a lit sphere: `lit.xyz` is the direction from the moon to the sun
// lighting it, which is what gives the disc a phase; a zero `lit.xyz` draws the full face
// unshaded. `lit.w > 0` marks a body with no solid surface, whose procedural face is latitude
// bands rather than the cratered maria. With TOY_SKY_BODY_ATLAS and rect.zw > 0 its face is the
// equirect image in the sky-body atlas at `rect`.
//
// Derivatives (fwidth) are taken before any return, while the whole quad still runs: the rim's
// anti-aliasing is one-to-two PIXELS wide at any apparent size -- a fixed fraction of the disc
// was a hundred-pixel blur on a giant filling half the sky and a shimmer on a far speck.
// Returns the disc's light in rgb and its COVERAGE in a: how fully this pixel sits on the
// body's face, phase or no phase. A new moon is black but still a body, so the caller draws
// stars and sun discs attenuated by (1 - max coverage) -- additive discs that let the star
// field shine through a planet read as holograms.
//
// `atmo` is the body's thin scattering shell: rgb its scatter tint, w its thickness as a
// fraction of the disc's radius (0 = airless, no shell). The shell is a tinted rim inside the
// limb plus a soft halo just past it, shaded with a wider terminator than the surface (the
// shell scatters well into its own twilight) and boosted when the body is backlit -- a dark
// disc in front of its sun wears a glowing ring, which is all of it that still shows.
vec4 sky_moon_disc(vec3 dir, vec4 moon, vec3 tint, float radiance, vec4 lit, vec4 rect,
                   vec4 atmo) {
    float cm = dot(dir, moon.xyz);
    float rad = max(acos(clamp(moon.w, -1.0, 1.0)), 1e-5);
    float r = acos(clamp(cm, -1.0, 1.0)) / rad;
    float px = clamp(fwidth(r), 1e-4, 0.5);
    vec3 m = moon.xyz;
    vec3 side = normalize(cross(m, abs(m.z) < 0.99 ? vec3(0, 0, 1) : vec3(1, 0, 0)));
    vec3 upv = cross(side, m);
    vec2 local = vec2(dot(dir - m, side), dot(dir - m, upv)) / rad;
    // The disc as the sphere it is: its outward normal at this texel, in the (side, upv, -m)
    // frame (-m is the hemisphere facing the viewer), lit by its own sun.
    float rr = min(dot(local, local), 1.0);
    float mu = sqrt(max(1.0 - rr, 0.0));
    vec3 n = side * local.x + upv * local.y - m * mu;
    float lon = atan(dot(n, side), -dot(n, m));
    float lat = asin(clamp(dot(n, upv), -1.0, 1.0));
    vec2 uv = vec2(0.5 + lon / 6.2831853, 0.5 - lat / 3.14159265);
#ifdef TOY_SKY_BODY_ATLAS
    vec2 slot_uv = rect.xy + rect.zw * uv;
    vec2 foot = fwidth(slot_uv);
#endif
    float halo = max(atmo.w, 0.0) * 1.5;   // how far past the limb the shell's glow reaches
    if (radiance <= 0.0 || r >= 1.0 + halo + 2.0 * px) return vec4(0.0);

    float edge = 1.0 - smoothstep(1.0 - px, 1.0 + px, r);
    float phase = dot(lit.xyz, lit.xyz) > 0.5 ? smoothstep(-0.08, 0.25, dot(n, lit.xyz)) : 1.0;
    // Gentle limb darkening, so a large disc reads as a ball rather than a flat sticker.
    float limb = 0.55 + 0.45 * pow(mu, 0.8);

    vec3 albedo;
    if (lit.w > 0.5) {
        // The banded fallback: band count and wobble hashed off the body's direction, so the
        // passes without the atlas binding still draw a giant as a giant.
        float hb = sky_hash13(floor(m * 37.0) + 3.0);
        float wave = sin(lat * (14.0 + 12.0 * hb) + (sky_vnoise(n * 3.5 + hb * 9.0) - 0.5) * 3.0);
        albedo = vec3(0.93 + 0.15 * wave);
    } else {
        albedo = vec3(mix(1.0, 0.62, sky_moon_maria(local)));
    }
#ifdef TOY_SKY_BODY_ATLAS
    if (rect.z > 0.0) {
        // The visible hemisphere of the body's equirect map, inset half a texel so slots never
        // bleed into their neighbours. Four rotated-grid taps over the pixel's footprint stand
        // in for the mip chain the atlas does not carry -- an un-prefiltered single tap
        // shimmers as soon as the disc is minified.
        vec2 inset = 0.5 / vec2(textureSize(u_sky_bodies, 0));
        vec2 lo = rect.xy + inset;
        vec2 hi = rect.xy + rect.zw - inset;
        vec2 fw = min(foot, rect.zw * 0.25);
        vec3 s = vec3(0.0);
        s += texture(u_sky_bodies, clamp(slot_uv + vec2( 0.125,  0.375) * fw, lo, hi)).rgb;
        s += texture(u_sky_bodies, clamp(slot_uv + vec2( 0.375, -0.125) * fw, lo, hi)).rgb;
        s += texture(u_sky_bodies, clamp(slot_uv + vec2(-0.125, -0.375) * fw, lo, hi)).rgb;
        s += texture(u_sky_bodies, clamp(slot_uv + vec2(-0.375,  0.125) * fw, lo, hi)).rgb;
        albedo = s * 0.25;
    }
#endif
    vec3 air = vec3(0.0);
    if (atmo.w > 1e-4) {
        // The shell's optical path peaks at the limb itself: a band rising toward the rim over
        // ~3 shell thicknesses of the face, falling off over the halo past it.
        float band = r < 1.0
                         ? smoothstep(1.0 - 3.0 * atmo.w, 1.0, r)
                         : pow(max(1.0 - (r - 1.0) / max(halo, 1e-4), 0.0), 2.0);
        float nl = dot(normalize(n), lit.xyz);
        float lit_w = dot(lit.xyz, lit.xyz) > 0.5 ? smoothstep(-0.45, 0.35, nl) : 1.0;
        // Backlit: the sun sits behind the body as seen from here, and forward scattering
        // lights the whole ring.
        float back = dot(lit.xyz, lit.xyz) > 0.5 ? pow(max(dot(lit.xyz, m), 0.0), 8.0) : 0.0;
        air = atmo.rgb * (radiance * band * (0.45 * lit_w + 1.6 * back));
    }
    return vec4(tint * albedo * (radiance * edge * phase * limb) + air, edge);
}

// -- The star backdrop -------------------------------------------------------------------------
//
// One seeded field over the whole celestial sphere: point stars in three magnitude layers, their
// density clustered and lifted along a galactic band that carries its own diffuse glow, dust
// lanes and nebulae, with a sparse scattering of far galaxies behind everything.
// lights.sky_backdrop orients and varies it: xyz is the band's pole (the sun directions' frame),
// w the variation seed; an xyz of zero keeps a built-in orientation.

// Octave offsets stay small on purpose: the whole backdrop keeps its noise domains within a few
// hundred units, because fract-based hashing visibly quantises on the GPU once inputs reach the
// thousands (hard rectangular seams in what should be smooth fBm).
float sky_fbm(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int o = 0; o < 4; ++o) {
        v += a * sky_vnoise(p);
        p = p * 2.03 + 5.1;
        a *= 0.5;
    }
    return v;
}

// A star's colour from one uniform hash: a blackbody-ish ramp -- warm-white middle dominating,
// with rare deep-red and blue-white ends.
vec3 sky_star_tint(float u) {
    if (u < 0.06) return mix(vec3(1.0, 0.42, 0.22), vec3(1.0, 0.80, 0.58), u / 0.06);
    if (u < 0.55) return mix(vec3(1.0, 0.80, 0.58), vec3(0.97, 0.95, 0.93), (u - 0.06) / 0.49);
    if (u < 0.94) return mix(vec3(0.97, 0.95, 0.93), vec3(0.85, 0.89, 1.0), (u - 0.55) / 0.39);
    return mix(vec3(0.85, 0.89, 1.0), vec3(0.6, 0.72, 1.0), (u - 0.94) / 0.06);
}

// One layer of point stars: at most one per cell of a grid over the direction cube, each a tiny
// gaussian point with its own brightness and colour. `density` is the chance a cell holds a
// star; `so` offsets the lattice so layers and seeds never line up. Every spread here must die
// out well inside 0.3 of a cell -- a pixel only evaluates its own cell, so anything wider is
// CUT at the cell border into a hard-edged wedge.
vec3 sky_star_layer(vec3 dir, float cells, float density, float size, float t, vec3 so) {
    vec3 c = floor(dir * cells + so);
    float h = sky_hash13(c + 17.0);
    if (h > density) return vec3(0.0);
    vec3 j = sky_hash33(c);
    vec3 star = normalize(c - so + 0.3 + 0.4 * j);   // kept off the cell's edges, which would cut it
    float ang = acos(clamp(dot(dir, star), -1.0, 1.0));
    float b = exp(-(ang * ang) / (size * size));
    // A thin bright tail over a faint majority -- closer to a real magnitude histogram than a
    // flat spread, so a few stars in any view stand well proud of the rest.
    float mag = pow(sky_hash13(c + 3.1), 7.0);
    float twinkle = 0.82 + 0.18 * sin(t * (2.0 + 5.0 * j.x) + 40.0 * j.y);
    return sky_star_tint(j.z) * b * (0.07 + 2.5 * mag) * twinkle;
}

// The hero stars: far sparser, brighter, bigger, each with a tight halo. These give one sky its
// few recognisable anchors. Halo sigma stays under ~0.09 of a cell so the falloff is gone long
// before the cell border (see sky_star_layer's cut warning).
vec3 sky_bright_star(vec3 dir, float t, vec3 so) {
    float cells = 26.0;
    vec3 c = floor(dir * cells + so);
    float h = sky_hash13(c + 23.0);
    if (h > 0.035) return vec3(0.0);
    vec3 j = sky_hash33(c + 5.0);
    vec3 star = normalize(c - so + 0.35 + 0.3 * j);
    float ang = acos(clamp(dot(dir, star), -1.0, 1.0));
    float size = 0.0010 + 0.0008 * j.y;
    float core = exp(-(ang * ang) / (size * size));
    float halo = exp(-(ang * ang) / (size * size * 9.0)) * 0.05;
    float b = 2.5 + 7.0 * pow(sky_hash13(c + 9.4), 2.0);
    float twinkle = 0.92 + 0.08 * sin(t * (1.5 + 3.0 * j.x) + 40.0 * j.z);
    return sky_star_tint(j.x) * (core + halo) * b * twinkle;
}

// Far galaxies: rare elliptical smudges tilted at random -- a brighter warm core inside a faint
// cool disc, squashing toward a sliver when seen edge-on. Sized so 3 sigma of the long axis
// stays inside the jitter's margin to the cell border.
vec3 sky_galaxy(vec3 dir, vec3 so) {
    float cells = 12.0;
    vec3 c = floor(dir * cells + so);
    float h = sky_hash13(c + 41.0);
    if (h > 0.05) return vec3(0.0);
    vec3 j = sky_hash33(c + 13.0);
    vec3 g = normalize(c - so + 0.35 + 0.3 * j);
    vec3 side = normalize(cross(g, abs(g.z) < 0.99 ? vec3(0, 0, 1) : vec3(1, 0, 0)));
    vec3 upv = cross(side, g);
    vec2 local = vec2(dot(dir - g, side), dot(dir - g, upv));
    float a = j.x * 6.2831853;
    vec2 e = vec2(cos(a) * local.x - sin(a) * local.y, sin(a) * local.x + cos(a) * local.y);
    e.y *= 2.0 + 4.0 * j.y;
    float size = 0.004 + 0.008 * j.z;
    float d2 = dot(e, e) / (size * size);
    return vec3(0.75, 0.8, 0.95) * exp(-d2) * 0.08 + vec3(1.0, 0.9, 0.75) * exp(-d2 * 6.0) * 0.2;
}

// The galactic band's diffuse glow -- a milky arch broken by fBm patchiness and darker dust
// lanes, brightest toward its core direction -- plus a few soft tinted nebulae that favour the
// band's neighbourhood.
vec3 sky_backdrop_glow(vec3 dir, vec3 pole, float band, vec3 so) {
    vec3 q = dir * 3.1 + so;
    float body = sky_fbm(q + sky_fbm(q * 1.7) * 1.4);
    float dust = 1.0 - 0.8 * smoothstep(0.38, 0.68, sky_fbm(dir * 5.3 + so * 1.31 + 7.0));
    vec3 core_dir = normalize(cross(pole, abs(pole.z) < 0.99 ? vec3(0, 0, 1) : vec3(1, 0, 0)));
    float core = 0.55 + 0.75 * smoothstep(-0.2, 1.0, dot(dir, core_dir));
    vec3 glow = vec3(0.78, 0.84, 1.0) * band * dust * core *
                smoothstep(0.30, 0.82, body) * 0.75;
    float n = sky_fbm(dir * 2.3 + so * 2.1 + 31.0);
    float neb = smoothstep(0.64, 0.82, n) * (0.25 + 0.75 * band) *
                (0.4 + 0.6 * sky_fbm(dir * 6.1 + so + 71.0));
    vec3 neb_col = mix(vec3(0.85, 0.35, 0.45), vec3(0.3, 0.68, 0.75),
                       smoothstep(0.3, 0.7, sky_vnoise(dir * 1.7 + so + 53.0)));
    return glow * 0.22 + neb_col * neb * 0.10;
}

// The whole backdrop at `dir`: three star layers over clustered density, the band's glow, the
// galaxies. Dimensionless linear radiance; callers scale it by star visibility and the air.
vec3 sky_star_field(vec3 dir, float t) {
    vec3 pole = lights.sky_backdrop.xyz;
    pole = dot(pole, pole) < 0.25 ? vec3(0.853, 0.201, 0.482) : normalize(pole);
    // The seed becomes a small lattice offset: wrapped to +-12 cells so every noise domain stays
    // within float precision (see sky_fbm's note) while still decorrelating seeds completely.
    vec3 so = (fract(lights.sky_backdrop.w * vec3(0.1031, 0.0719, 0.0423)) - 0.5) * 24.0;
    float lat = dot(dir, pole);
    float band = exp(-(lat * lat) / (2.0 * 0.21 * 0.21));
    // Clustering: a low-frequency density field, squared for real voids between the knots, then
    // lifted further inside the band. Off the band the sky runs far sparser than inside it,
    // which is the structure a uniform field lacked.
    float cf = sky_fbm(dir * 2.6 + so * 0.5 + 19.0);
    float cluster = 0.25 + 2.4 * cf * cf;
    float dmod = clamp(cluster * (1.0 + 1.8 * band), 0.1, 3.0);
    return sky_star_layer(dir, 260.0, 0.05 * dmod, 0.00075, t, so)
         + sky_star_layer(dir, 90.0, 0.08 * dmod, 0.0008, t * 0.7, so * 1.7) * 2.5
         + sky_bright_star(dir, t, so)
         + sky_galaxy(dir, so)
         + sky_backdrop_glow(dir, pole, band, so);
}

// The cloud layer's colour (rgb) and transmittance (a) at a full-resolution sky pixel: a bilinear
// upsample of the reconstructed layer (sky_cloud_resolve.frag) over the texels whose block holds
// sky (a texel marched only to geometry has clipped clouds). The layer fills the top-left
// `scale` fraction of its half-resolution target (cloud_quality; the same rounding as
// SkyCloudPass::scaled_extent).
vec4 sky_clouds_upsample(vec2 uv, float scale) {
    ivec2 size = clamp(ivec2(floor(vec2(textureSize(u_sky_clouds, 0)) * scale + 0.5)), ivec2(1), textureSize(u_sky_clouds, 0));
    vec2 p = uv * vec2(size) - 0.5;
    ivec2 i = ivec2(floor(p));
    vec2 f = p - vec2(i);
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = 0; y <= 1; ++y) {
        for (int x = 0; x <= 1; ++x) {
            ivec2 q = clamp(i + ivec2(x, y), ivec2(0), size - 1);
            vec4 s = texelFetch(u_sky_clouds, q, 0);
            float w = (x == 1 ? f.x : 1.0 - f.x) * (y == 1 ? f.y : 1.0 - f.y);
            if (s.a < 0.0 || !cloud_sky(s.a)) continue;
            sum += vec4(s.rgb, cloud_trans(s.a)) * w;
            wsum += w;
        }
    }
    return wsum > 1e-4 ? sum / wsum : vec4(0.0, 0.0, 0.0, 1.0);
}

vec3 sky_physical(vec3 dir, vec2 uv) {
    SkyAtmosphere atmo;
    atmo.ozone.w = 6360.0;
    atmo.ground.w = 6460.0;
    float view_r = sky_view_radius(atmo, camera.camera_pos.z);
    vec3 sun = lights.sky_sun.xyz;
    vec3 sky = sky_view_lookup(u_sky_view, atmo, view_r, dir, sun) * lights.sky_params.w;
    // Night airglow / light pollution floor, so a moonless night is not pure black.
    sky += vec3(0.55, 0.75, 1.3) * 1e-3 * lights.sky_extra.z;

    bool above = sky_ray_sphere(vec3(0.0, 0.0, view_r), dir, atmo.ozone.w) < 0.0;
    if (above) {
        vec3 t_view = sky_transmittance(u_sky_transmittance, atmo, view_r, dir.z);
        // The moon discs first: their coverage then occludes the stars and the sun discs behind
        // them -- a body is a body, not a hologram the backdrop shines through. The arrays when
        // the frame carries any (sky_body_counts), the single built-in pair otherwise; a legacy
        // moon is a full disc -- always opposite the sun, so lit straight on (zero lit).
        vec4 moons = vec4(0.0);
        if (lights.sky_body_counts.y > 0.5) {
            int count = int(lights.sky_body_counts.y + 0.5);
            for (int i = 0; i < 8; ++i) {
                if (i >= count) break;
                vec4 d = sky_moon_disc(dir, lights.sky_moon_dirs[i],
                                       lights.sky_moon_colors[i].rgb,
                                       lights.sky_moon_colors[i].w,
                                       lights.sky_moon_lit[i],
                                       lights.sky_moon_rects[i],
                                       lights.sky_moon_atmo[i]);
                moons.rgb += d.rgb;
                moons.a = max(moons.a, d.a);
            }
        } else {
            moons = sky_moon_disc(dir, lights.sky_moon, vec3(0.95, 0.96, 1.0),
                                  lights.sky_extra.y, vec4(0.0), vec4(0.0), vec4(0.0));
        }
        float open = 1.0 - moons.a;
        float star_vis = lights.sky_params.z;
        if (star_vis > 0.0) {
            sky += sky_star_field(dir, lights.sky_extra.w) * star_vis * t_view * (0.03 * open);
        }
        if (lights.sky_body_counts.x > 0.5) {
            int suns = int(lights.sky_body_counts.x + 0.5);
            for (int i = 0; i < 4; ++i) {
                if (i >= suns) break;
                sky += t_view * open * sky_sun_disc(dir, lights.sky_sun_dirs[i],
                                                    lights.sky_sun_colors[i].rgb,
                                                    lights.sky_sun_colors[i].w);
            }
        } else {
            sky += t_view * open * sky_sun_disc(dir, lights.sky_sun, vec3(1.0), lights.sky_extra.x);
        }
        sky += t_view * moons.rgb;
    }
    return sky;
}

// The sky with no atmosphere at all (render sky_model: space): stars at full strength in every
// direction -- there is no daytime air to wash them out of, which is why
// Engine::sync_sky_render_state_()'s space branch sets sky_params.z (star_vis) to a flat 1 --
// plus the same sun and moon discs sky_physical() draws, minus every atmosphere term (no
// sky-view LUT lookup, no transmittance, no "above the shell" gate: in vacuum a disc is exactly
// as bright looking straight at either pole as anywhere else). The near-black background itself
// is whatever the scene authored in sky_zenith/horizon/ground -- this function only adds to it.
vec3 sky_space(vec3 dir) {
    vec3 sky = vec3(0.0);
    // The moon discs first: their coverage occludes the stars and the sun discs behind them --
    // a body is a body, not a hologram the backdrop shines through. The arrays when the frame
    // carries any (sky_body_counts), the single built-in pair otherwise; a legacy moon is a
    // full disc -- always opposite the sun, so lit straight on (zero lit).
    vec4 moons = vec4(0.0);
    if (lights.sky_body_counts.y > 0.5) {
        int count = int(lights.sky_body_counts.y + 0.5);
        for (int i = 0; i < 8; ++i) {
            if (i >= count) break;
            vec4 d = sky_moon_disc(dir, lights.sky_moon_dirs[i], lights.sky_moon_colors[i].rgb,
                                   lights.sky_moon_colors[i].w, lights.sky_moon_lit[i],
                                   lights.sky_moon_rects[i], lights.sky_moon_atmo[i]);
            moons.rgb += d.rgb;
            moons.a = max(moons.a, d.a);
        }
    } else {
        moons = sky_moon_disc(dir, lights.sky_moon, vec3(0.95, 0.96, 1.0), lights.sky_extra.y,
                              vec4(0.0), vec4(0.0), vec4(0.0));
    }
    float open = 1.0 - moons.a;
    float star_vis = lights.sky_params.z;
    if (star_vis > 0.0) {
        sky += sky_star_field(dir, lights.sky_extra.w) * star_vis * open;
    }
    if (lights.sky_body_counts.x > 0.5) {
        int suns = int(lights.sky_body_counts.x + 0.5);
        for (int i = 0; i < 4; ++i) {
            if (i >= suns) break;
            sky += open * sky_sun_disc(dir, lights.sky_sun_dirs[i], lights.sky_sun_colors[i].rgb,
                                       lights.sky_sun_colors[i].w);
        }
    } else {
        sky += open * sky_sun_disc(dir, lights.sky_sun, vec3(1.0), lights.sky_extra.x);
    }
    sky += moons.rgb;
    return sky;
}

// A sky pixel's colour with the volumetric cloud layer over it (sky_params.y > 0: the fraction
// of the cloud target the march filled), whichever sky model drew `sky`.
vec3 sky_with_clouds(vec3 sky, vec2 uv) {
    if (lights.sky_params.y > 0.0) {
        vec4 c = sky_clouds_upsample(uv, lights.sky_params.y);
        sky = sky * c.a + c.rgb;
    }
    return sky;
}

#endif // TOY_SKY_PHYSICAL_GLSL
