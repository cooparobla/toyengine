// hair_surface_frag.glsl -- the FRAGMENT half of the `hair` derived shader: the lengthways
// highlight a strand has, and the strand-to-strand variation that stops a groom reading as one
// flat mass. Included only by hair.frag; the vertex half is hair_surface.glsl.
//
// ### Why this is an approximation, and which one
//
// Real hair is an anisotropic BSDF (Kajiya-Kay, Marschner): its specular is a BAND running across
// the strand rather than a round dot. A deferred G-buffer cannot express that -- G1..G4 carry
// albedo/AO, normal/metallic, position/roughness, emissive/toon and velocity, with no channel for
// a tangent direction or an anisotropy strength, and adding one would cost every pass in the
// engine a wider attachment for one material's benefit.
//
// So the highlight is produced geometrically instead, with no change to the lighting pass: the
// shading normal is turned toward the direction a round strand's surface faces the viewer from --
// the component of the view vector perpendicular to the strand. Every point along one strand then
// presents the same cross-sectional normal to the eye, which is exactly the condition that makes
// an isotropic highlight stretch into a band along the strand. It is the same trick hair cards
// use in a deferred renderer, and it is why hair_data.cpp bakes the strand direction into
// `tangent.xyz`: tbn[0] is that direction, Gram-Schmidt'd against the normal.
//
// The turn is partial. Replacing the normal outright would make every strand in a lock shade
// identically, losing the baked cylindrical variation that reads as individual strands.

void gfx_surface_fragment(inout GfxSurface s) {
    // tbn[0] is the strand's direction in world space; tangent.w stayed a real handedness so the
    // frame here is the ordinary one.
    vec3 T = s.tbn[0];
    float t_len = length(T);
    if (t_len < 1e-4) return;
    T /= t_len;

    vec3 V = camera.camera_pos - s.position_ws;
    float v_len = length(V);
    if (v_len < 1e-4) return;
    V /= v_len;

    // The view direction with the along-strand part removed: where a cylinder's surface faces the
    // eye. Degenerate only when looking straight down a strand, where there is no cross section
    // to speak of and the baked normal is already the right answer.
    vec3 perpendicular = V - T * dot(V, T);
    float p_len = length(perpendicular);
    if (p_len > 1e-3) {
        s.normal_ws = normalize(mix(s.normal_ws, perpendicular / p_len, 0.65));
    }

    // Strand-to-strand variation, from the per-strand random in uv.y (see hair_data.h on why the
    // random rides there rather than in tangent.w). Darkening some strands and roughening others
    // is what separates a groom into visible hairs instead of a single glossy shell.
    float jitter = s.uv.y;
    s.albedo    *= mix(0.78, 1.12, jitter);
    s.roughness  = clamp(s.roughness * mix(0.85, 1.15, fract(jitter * 7.3)), 0.03, 1.0);
}
