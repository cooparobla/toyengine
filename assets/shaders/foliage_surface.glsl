// foliage_surface.glsl -- vegetation that bends in the weather's wind: grass tufts, bushes, tree
// canopies, leaf cards. The only file with real code in the "foliage" derived shader;
// foliage.vert / foliage.tese and their shadow twins are includers of it over the G-buffer and
// shadow backbones, so a plant's shadow sways in lockstep with the plant itself (see
// gfx/surface/shadow_vs.glsl's file doc for why that consistency is load-bearing).
//
// The wind is the WORLD's: gfx_world_wind_at() at the plant's root (m/s; the weather's mean wind
// plus gust fronts rolling downwind -- the World tab / conditions drive it; see
// toyengine/weather/wind.h). No wind, no sway: a calm day is still. The material only says how
// the plant responds -- shader_params (gfx_params):
//
//   x = flexibility  -- tip sway, as a fraction of the plant's height, in a 10 m/s wind (about
//                       0.4 of that in a light 1.5 m/s breeze) -- grass ~0.35, bushes ~0.1,
//                       tree canopies ~0.04
//   y = frequency    -- rad/s of the flutter (it speeds up with the wind)
//   z = height       -- the plant's height in its mesh's own units (the sway grows from 0 at the
//                       root to full at this height); 0 = 1
//   w = up axis      -- 0: the mesh's +Z is up (this engine's convention, every nature mesh);
//                       1: its +Y is up (a flat card authored on the XY plane and stood up by its
//                       transform, e.g. the plane primitive rotated 90 degrees)
//
// The motion is a steady lean downwind plus a flutter whose phase travels across the world in
// the wind's direction -- gusts visibly roll over a field -- with a smaller sideways component,
// all scaled by the square of the height fraction so roots stay planted and tips move most.

void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    vec2 base = v.model[3].xy;   // the plant's root: every vertex of one plant shares the wind
    vec2 wind = gfx_world_wind_at(v.model[3].xyz).xy;
    float speed = length(wind);
    if (speed < 1e-3) return;
    vec2 dir = wind / speed;

    // Height fraction along the mesh's up axis, and the plant's height in world metres (the
    // instance's own scale along that axis -- a big tree sways further than a small one).
    bool y_up = gfx_params.w > 0.5;
    float height_os = gfx_params.z > 0.0 ? gfx_params.z : 1.0;
    float up_os = y_up ? v.position_os.y : v.position_os.z;
    float t = clamp(up_os / height_os, 0.0, 1.0);
    float scale = length(y_up ? v.model[1].xyz : v.model[2].xyz);
    float height_ws = height_os * scale;
    float bend = t * t;

    // Strength rises fast in light airs -- grass visibly stirs in a 1-2 m/s breeze -- and keeps
    // growing, more slowly, toward storms (a plant only bends so far): sqrt of the speed, soft-
    // capped. About 0.4 of the 10 m/s response at 1.5 m/s, 1 at 10 m/s, 1.3 at 20 m/s.
    float s = speed / 10.0;
    float response = sqrt(s) / (1.0 + 0.25 * max(s - 1.0, 0.0));
    float push = gfx_params.x * height_ws * response;

    // The flutter: a travelling wave (phase moves downwind) at a rate that rises with the wind,
    // plus a slower swell, so a field ripples instead of swaying in unison (the gust fronts in
    // the wind itself add the big, slow waves).
    float rate = gfx_params.y * (0.6 + 0.4 * min(s, 2.0));
    float along = dot(base, dir);
    float phase = gfx_time.x * rate - along * 0.35;
    float swell = 0.5 + 0.5 * sin(gfx_time.x * 0.6 - along * 0.05);
    float flutter = sin(phase) * 0.35 + sin(phase * 2.3 + 1.7) * 0.15;
    vec2 side = vec2(-dir.y, dir.x);
    float sideways = sin(phase * 1.3 + dot(base, side) * 0.5) * 0.2;

    // Light airs: mostly flutter (grass waves in place); strong wind: mostly a sustained lean.
    float lean = mix(0.25, 0.6, clamp(s, 0.0, 1.0));
    vec2 offset = (dir * (lean + 0.4 * swell + flutter * 1.4) + side * sideways) * push * bend;
    v.position_ws.xy += offset;
    // Keep the stem roughly its length: a leaning tip also drops a little.
    v.position_ws.z -= dot(offset, offset) / max(2.0 * height_ws, 1e-3) * t;
}
