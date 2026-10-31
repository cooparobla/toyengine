// hair_surface.glsl -- the VERTEX half of the `hair` derived shader: strands bending in the
// world's wind. Shared verbatim by hair.vert, hair_shadow.vert and hair_shadow_cube.vert, so a
// groom's shadow sways in lockstep with the groom (see gfx/surface/shadow_vs.glsl's file doc for
// why a shadow entry point left at stock is a bug, not an optimisation).
//
// The fragment half is a SEPARATE file (hair_surface_frag.glsl). It has to be: it reads the
// camera UBO, which only the G-buffer backbone declares, so including it from a shadow vertex
// entry point would not compile.
//
// This is what animates a groom that is NOT simulated, and it costs nothing per frame -- the mesh
// is static and the motion is entirely here. A simulated groom gets its motion from the solver
// instead and normally runs with wind_flexibility at 0.
//
// The wind is the WORLD's: gfx_world_wind_at() at the groom's origin (m/s; the weather's mean
// wind plus gust fronts, plus any local influence reaching it -- a rotor's downwash, a passing
// sword; see toyengine/weather/wind.h and toyengine/env/README.md). No wind, no motion: a calm
// day is still, which is why a hair demo scene has to ask for a breeze.
//
// shader_params (gfx_params), written by HairSystem from the component:
//
//   x = flexibility -- tip sway as a fraction of the strand's own length in a 10 m/s wind
//   y = frequency   -- rad/s of the flutter (it speeds up with the wind)
//   z = length      -- the groom's strand length in metres, in the mesh's own units
//   w = spare
//
// ### Why the per-strand phase comes from uv.y
//
// A groom is ONE mesh with ONE model matrix, so unlike foliage -- where every card is a separate
// instance and can hash its own root out of `model` -- every strand here shares `v.model`. There
// is nothing in the transform to tell two strands apart. The per-strand random that
// hair_data.cpp bakes into `uv.y` is what decorrelates them, and without it a whole head of hair
// would flutter as one rigid sheet.

void gfx_surface_vertex(inout GfxSurfaceVertex v) {
    // uv.x is 0 at the root and 1 at the tip. It is the one channel every backbone declares,
    // which is why the sway keys off it rather than reconstructing a height from the position.
    float t = clamp(v.uv.x, 0.0, 1.0);
    if (t <= 0.0) return;   // the root is pinned to the scalp by definition

    vec3 wind3 = gfx_world_wind_at(v.model[3].xyz);
    vec2 wind = wind3.xy;
    float speed = length(wind);
    // A downward local wind flattens hair even where it barely leans -- straight under a rotor,
    // where the outward fan cancels. The weather's own wind is always horizontal, so this is
    // exactly zero in a scene with no local source.
    float press = clamp(-wind3.z / 12.0, 0.0, 1.0);
    if (speed < 1e-3 && press <= 0.0) return;
    vec2 dir = speed > 1e-3 ? wind / speed : vec2(0.0);

    float flex = gfx_params.x;
    float freq = max(gfx_params.y, 0.0);
    float len  = gfx_params.z > 0.0 ? gfx_params.z : 1.0;
    // The groom's world-space length: a scaled-up head sways proportionally further.
    float length_ws = len * length(v.model[0].xyz);

    // Strength rises fast in light airs -- hair stirs in a 1-2 m/s breeze -- then keeps growing
    // more slowly toward a gale, because a strand only bends so far. About 0.4 of the 10 m/s
    // response at 1.5 m/s, 1 at 10 m/s, 1.3 at 20 m/s. Same curve the foliage shader uses, so a
    // head of hair and the grass beside it answer one gust the same way.
    float s = speed / 10.0;
    float response = sqrt(s) / (1.0 + 0.25 * max(s - 1.0, 0.0));
    float push = flex * length_ws * response;

    // Quadratic along the strand: the root stays planted and the tip moves most.
    float bend = t * t;

    // The flutter's phase travels downwind across the world, so a gust visibly rolls over a head
    // rather than pulsing everywhere at once; uv.y then offsets each strand within that.
    float jitter = v.uv.y;
    float rate = freq * (0.6 + 0.4 * min(s, 2.0));
    float phase = gfx_time.x * rate * (0.8 + 0.4 * jitter)
                - dot(v.model[3].xy, dir) * 0.35
                + jitter * 6.2831853;
    float flutter = sin(phase) * 0.3 + sin(phase * 2.3 + 1.7) * 0.12;
    vec2  side = vec2(-dir.y, dir.x);
    float sideways = sin(phase * 1.3 + jitter * 4.0) * 0.18;

    // Light airs: mostly flutter, hair stirring in place. Strong wind: mostly a sustained lean.
    float lean = mix(0.3, 0.75, clamp(s, 0.0, 1.0));
    vec2 offset = (dir * (lean + flutter) + side * sideways) * (push * bend);

    v.position_ws.xy += offset;
    // Arc shortening: a strand that leans sideways has not also grown. Dropping the tip by the
    // chord's shortfall keeps the strand the length it was authored at.
    v.position_ws.z -= dot(offset, offset) / max(2.0 * length_ws, 1e-3);
    // A downward wind presses the whole strand toward the scalp.
    v.position_ws.z -= length_ws * bend * press * max(flex, 0.05) * 1.5;
}
