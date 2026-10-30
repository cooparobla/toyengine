// foliage_surface.glsl -- vegetation that bends in the weather's wind: grass tufts, bushes, tree
// canopies, leaf cards. The only file with real code in the "foliage" derived shader;
// foliage.vert / foliage.tese and their shadow twins are includers of it over the G-buffer and
// shadow backbones, so a plant's shadow sways in lockstep with the plant itself (see
// gfx/surface/shadow_vs.glsl's file doc for why that consistency is load-bearing).
//
// The wind is the WORLD's: gfx_world_wind_at() at the plant's root (m/s; the weather's mean wind
// plus gust fronts rolling downwind, plus any local environmental influence reaching the plant --
// a rotor's downwash, a sword swing; see toyengine/weather/wind.h and toyengine/env/README.md).
// No wind, no sway: a calm day is still. A local wind with a DOWNWARD component also presses the
// plant flat, which the weather's always-horizontal wind never does. The material only says how
// the plant responds -- shader_params (gfx_params):
//
//   x = flexibility  -- tip sway, as a fraction of the plant's height, in a 10 m/s wind (about
//                       0.4 of that in a light 1.5 m/s breeze) -- grass ~0.35, bushes ~0.1,
//                       tree canopies ~0.04
//   y = frequency    -- rad/s of the flutter (it speeds up with the wind)
//   z = height       -- the plant's height in its mesh's own units (the sway grows from 0 at the
//                       root to full at this height); 0 = 1
//   w = up axis      -- which mesh axis runs from the plant's root to its tip. 0: +Z (this
//                       engine's convention, every nature mesh); 1: +Y (a flat card authored on
//                       the XY plane and stood up by its transform, e.g. the plane primitive
//                       rotated 90 degrees); 2: +X (a card or frond authored along +X and aimed
//                       by its transform, e.g. a leaf cluster on a twig)
//
// The motion is a steady lean downwind plus a flutter whose phase travels across the world in
// the wind's direction -- gusts visibly roll over a field -- with a smaller sideways component,
// all scaled by the square of the height fraction so roots stay planted and tips move most.
//
// Each PLANT also gets its own flutter phase and its own slightly different rate. Without that,
// every card instanced from one mesh shares a phase (they differ only by the travelling wave's
// `along` term, which over the 10 cm between two leaves on a twig is nothing) and a canopy
// flutters as one rigid blob. The scatter is applied to the flutter and the sideways swing only,
// NOT to the lean or the swell: those two are what make a gust front visibly roll across a
// field, and they have to stay spatially coherent to read as wind rather than as noise.

/**
 * @brief A stable 0..1 random from a world position -- one value per plant, constant over time.
 *
 * The instance origin is the only thing that distinguishes one card from the next: same mesh,
 * same material, same draw call.
 *
 * Keyed on XY only, deliberately. A leaf card has to reproduce its PARENT trunk's phase, and all
 * it can recover of that trunk is where its root sits -- in_foliage.zw carries the root's offset
 * in the ground plane, not its height. Hashing xyz would make the card's idea of its parent's
 * jitter differ from the trunk's own and put the two permanently out of step. Plants are
 * distinguished by where they stand, so XY loses nothing.
 */
float gfx_foliage_hash(vec2 p) {
    vec3 q = fract(vec3(p.x, p.y, p.x) * 0.1031);
    q += dot(q, q.zyx + 31.32);
    return fract((q.x + q.y) * q.z);
}

/**
 * @brief One plant's wind offset in the ground plane, as a multiple of its own push distance.
 *
 * Shared by a plant and by anything pinned to it, so a leaf card can reproduce the exact
 * displacement of the twig it sits on. `root` is the plant's own root in world XY: it sets both
 * the travelling wave's phase and the plant's jitter, which is why a card must recover its
 * parent's root rather than substitute its own.
 */
vec2 gfx_foliage_offset(vec2 root, vec2 dir, float s, float freq, float turbulence) {
    float jitter = gfx_foliage_hash(root);
    float rate = freq * (0.6 + 0.4 * min(s, 2.0)) * (1.0 + turbulence);
    float along = dot(root, dir);
    // Per-plant: a full random phase offset, and a rate within +/-25% of the material's, so
    // neighbours drift in and out of step instead of beating together forever.
    float phase = gfx_time.x * rate * (0.75 + 0.5 * jitter) - along * 0.35 + jitter * 6.2831853;
    // The swell keeps the SHARED phase: this is the slow wave that rolls downwind over everything.
    float swell = 0.5 + 0.5 * sin(gfx_time.x * 0.6 - along * 0.05);
    float flutter = sin(phase) * 0.35 + sin(phase * 2.3 + 1.7) * 0.15;
    vec2 side = vec2(-dir.y, dir.x);
    float sideways = sin(phase * 1.3 + dot(root, side) * 0.5 + jitter * 4.0) * 0.2;
    // Light airs: mostly flutter (grass waves in place); strong wind: mostly a sustained lean.
    float lean = mix(0.25, 0.6, clamp(s, 0.0, 1.0));
    return dir * (lean + 0.4 * swell + flutter * 1.4) + side * sideways;
}

void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    vec2 base = v.model[3].xy;   // the plant's root: every vertex of one plant shares the wind
    vec3 wind3 = gfx_world_wind_at(v.model[3].xyz);
    vec2 wind = wind3.xy;
    float speed = length(wind);
    // A downward local wind presses the plant flat even where it barely leans (straight under a
    // rotor, where the outward fan cancels). The weather's wind is always horizontal, so this
    // is exactly zero in a scene with no local source.
    float press = clamp(-wind3.z / 12.0, 0.0, 1.0);
    if (speed < 1e-3 && press <= 0.0) return;
    vec2 dir = speed > 1e-3 ? wind / speed : vec2(0.0);

    // Height fraction along the mesh's up axis, and the plant's height in world metres (the
    // instance's own scale along that axis -- a big tree sways further than a small one).
    int up_axis = int(gfx_params.w + 0.5);
    float height_os = gfx_params.z > 0.0 ? gfx_params.z : 1.0;
    float up_os = up_axis == 1 ? v.position_os.y : (up_axis == 2 ? v.position_os.x : v.position_os.z);
    float t = clamp(up_os / height_os, 0.0, 1.0);
    vec3 up_col = up_axis == 1 ? v.model[1].xyz : (up_axis == 2 ? v.model[0].xyz : v.model[2].xyz);
    float scale = length(up_col);
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
    float turbulence = gfx_env_turbulence_at(v.model[3].xyz);
    vec2 offset = gfx_foliage_offset(base, dir, s, gfx_params.y, turbulence) * push * bend;
    v.position_ws.xy += offset;

    // The PARENT's displacement, for a card pinned to a plant whose mesh bends (see
    // data::InstanceData::foliage). This card's world matrix was baked once and cannot follow
    // the twig it sits on, so it reproduces the twig's motion here and carries the whole card --
    // stem included -- along with it. Without this a trunk sways out from under its own leaves
    // and strips its branches bare; with it, `bend` above is purely the leaf's own flutter about
    // a stem that stays put.
    //
    // x is already its parent's flexibility * height * the parent's bend AT THIS CARD, so only
    // the wind response is left to apply. zw is the parent root's offset from this card, which
    // is what makes the phase and the travelling wave agree with the trunk's instead of being a
    // near miss -- a card 2 m out from the trunk is otherwise 0.7 rad out of step with it.
    if (gfx_instance_foliage.x > 0.0) {
        // The wind is sampled at the PARENT's root, not at this card. A local influence is a
        // SHAPE in the world -- the tree demo's gust is a 2.8 m column -- so a card out at the
        // edge of a canopy stands in weaker air than the trunk it hangs off, and reusing the
        // card's own wind under-compensates exactly where the branches are longest and the
        // error shows most. The trunk samples at its root; so must anything tracking it.
        vec3 parent_pos = vec3(v.model[3].xy + gfx_instance_foliage.zw, v.model[3].z);
        vec3 parent_wind = gfx_world_wind_at(parent_pos);
        float parent_speed = length(parent_wind.xy);
        vec2 parent_dir = parent_speed > 1e-3 ? parent_wind.xy / parent_speed : vec2(0.0);
        float parent_reach = gfx_instance_foliage1.x;                  // its height * its bend here
        float parent_height = max(gfx_instance_foliage1.y, 1e-3);
        float parent_t = sqrt(clamp(parent_reach / parent_height, 0.0, 1.0));
        if (parent_speed > 1e-3) {
            float ps = parent_speed / 10.0;
            float parent_response = sqrt(ps) / (1.0 + 0.25 * max(ps - 1.0, 0.0));
            vec2 parent_offset = gfx_foliage_offset(parent_pos.xy, parent_dir, ps,
                                                    gfx_instance_foliage.y,
                                                    gfx_env_turbulence_at(parent_pos))
                                 * (gfx_instance_foliage.x * parent_response);
            v.position_ws.xy += parent_offset;
            // Its leaning tip drops a little, exactly as the trunk's own does.
            v.position_ws.z -= dot(parent_offset, parent_offset) / (2.0 * parent_height) * parent_t;
        }
        // A DOWNWARD wind presses the parent flat. That motion scales with the parent's REACH
        // and not at all with its flexibility, which is why it needs foliage1: the product in
        // `foliage.x` cannot be taken apart. Without this a gust with a negative z pushes a
        // whole canopy over and down while its cards stay where they were -- the trunk, branches
        // and twigs lean out from under their own leaves.
        float parent_press = clamp(-parent_wind.z / 12.0, 0.0, 1.0);
        if (parent_press > 0.0) {
            float parent_drop = parent_press * parent_reach * 0.75;
            v.position_ws.z -= parent_drop;
            if (parent_speed > 1e-3) v.position_ws.xy += parent_dir * (parent_drop * 0.6);
        }
    }
    // Keep the stem roughly its length: a leaning tip also drops a little.
    v.position_ws.z -= dot(offset, offset) / max(2.0 * height_ws, 1e-3) * t;

    // Pressed flat: pull the tip down toward the root and fold it outward by what it loses in
    // height, so the blade bends over rather than shrinking. The horizontal wind's own lean
    // above already aims it; `press` only decides how far down it goes.
    if (press > 0.0) {
        float drop = press * bend * height_ws * 0.75;
        v.position_ws.z -= drop;
        if (speed > 1e-3) v.position_ws.xy += dir * (drop * 0.6);
    }
}
