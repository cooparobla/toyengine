#version 450

// XrayPass's mask stage -- see toyengine/render/passes/xray_pass.h.
//
// One pipeline, two roles, selected by params.x, so the subject draws and the permitted-
// occluder draws share a bind. Each writes a different channel group of one RGBA8 mask:
//
//   R  subject is HIDDEN here          (its depth is behind the G-buffer's)
//   G  a permitting occluder is the VISIBLE surface here
//   B  subject COVERS here             (hidden or not -- the outline's shape)
//   A  closeness: 1 right behind the covering surface, falling to 0 at params.w metres
//
// The composite needs both R and G at a pixel before it draws anything, which is what makes
// occluder opt-in work: a subject hidden behind an unflagged pillar gets R=1, G=0 and stays
// hidden. Keeping the raw closeness in A rather than pre-applying the fade is what leaves
// xray_depth_fade a composite-only push constant, free to change per frame.
//
// Blended with BlendMode::Max, so overlapping subject meshes UNION into one shape -- one tag
// on a prefab root covers its whole subtree, and a rig's limbs read as a single silhouette.
// Max rather than the alternatives because Additive scales rgb by the source alpha and then
// sums (saturating the flags and double-counting A), and None would depend on draw order.
//
// Vertex stage is the stock transparent.vert, so the first 64 push-constant bytes are its
// block; the mesh's own material is never read here.

layout(location = 0) in vec3 frag_world_pos;
layout(location = 1) in vec3 frag_world_normal;
layout(location = 2) in vec2 frag_uv;
layout(location = 3) in mat3 frag_TBN;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
    mat4 prev_view;
    mat4 prev_proj;
    vec4 jitter_ndc;
} camera;

layout(set = 1, binding = 0) uniform sampler2D u_scene_depth;

layout(push_constant) uniform XrayMaskPC {
    // transparent.vert's block (unread here, declared to match the shared range exactly).
    vec4  albedo;
    float metallic;
    float roughness;
    float ao;
    float alpha_cutoff;
    vec4  gfx_time;
    vec4  gfx_params;
    // Ours.
    vec4  params;   // x: role (1 subject, 2 occluder), yz: 1 / target size, w: 1 / gap metres
    vec4  depth;    // x: near, y: far, z: 1 = perspective, w: spare
} pc;

layout(location = 0) out vec4 out_mask;

// View-space distance from a [0,1] depth-buffer value.
float linear_depth(float z) {
    if (pc.depth.z < 0.5) return mix(pc.depth.x, pc.depth.y, z);   // orthographic
    const float d = pc.depth.y - z * (pc.depth.y - pc.depth.x);
    return d > 1e-6 ? (pc.depth.x * pc.depth.y) / d : pc.depth.y;
}

void main() {
    const vec2  uv      = gl_FragCoord.xy * pc.params.yz;
    const float scene_z = texture(u_scene_depth, uv).r;
    const int   role    = int(pc.params.x + 0.5);

    if (role == 2) {
        // An occluder grants permission only where it is the surface actually on screen. The
        // slack is one-sided and relative: this is the same geometry the G-buffer drew, so the
        // two depths agree to float noise, but TAA jitters the projection and the G-buffer is
        // sampled NEAREST, so an exact compare drops a texel-wide rim along every silhouette.
        if (gl_FragCoord.z > scene_z + 1e-5) discard;   // something else is in front
        out_mask = vec4(0.0, 1.0, 0.0, 0.0);
        return;
    }

    // Subject. Coverage (B) is written whether or not this fragment is hidden, and that is what
    // B is FOR: an outline traced from R instead would also fire along the hidden/visible
    // frontier, drawing a line across the middle of the player wherever the occluder's edge
    // crosses it. Traced from B the line only ever follows the subject's own silhouette.
    const bool hidden = gl_FragCoord.z > scene_z + 1e-5;
    if (!hidden) {
        out_mask = vec4(0.0, 0.0, 1.0, 0.0);   // covered, not hidden
        return;
    }

    // How far behind the covering surface this fragment sits, in metres.
    const float gap       = max(linear_depth(gl_FragCoord.z) - linear_depth(scene_z), 0.0);
    const float closeness = clamp(1.0 - gap * pc.params.w, 0.0, 1.0);
    out_mask = vec4(1.0, 0.0, 1.0, closeness);
}
