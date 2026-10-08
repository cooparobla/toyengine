#version 450

// ParticlePass's vertex stage -- see toyengine/render/passes/particle_pass.h. No vertex buffer:
// each instance (toy::render::ParticleInstance, five vec4s) becomes a quad of six vertices built
// from gl_VertexIndex and oriented by the batch's render mode (toy::render::ParticleRenderMode).

layout(location = 0) in vec4 i_pos_size;      // xyz world position, w size (quad height)
layout(location = 1) in vec4 i_color;         // linear rgba
layout(location = 2) in vec4 i_velocity_rot;  // xyz world velocity, w in-plane rotation (rad)
layout(location = 3) in vec4 i_orient;        // quaternion (x, y, z, w)
layout(location = 4) in vec4 i_misc;          // x age 0..1, y random 0..1, z flipbook frame

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

layout(push_constant) uniform ParticlePC {
    vec4 mode;     // x render mode, y sprite, z flipbook cols, w flipbook rows
    vec4 shading;  // x lit, y toon bands, z emissive, w additive
    vec4 shape;    // x softness, y soft distance, z camera fade, w aspect
    vec4 stretch;  // x stretch speed, y stretch length, z distortion, w time
    vec4 misc;     // x opacity, y has texture, z pivot (sizes), w is perspective
    vec4 depth;    // x near, y far, zw 1 / render extent
    vec4 ambient;  // x sky/ambient scale, y receive shadows
    vec4 extra;    // x pass, y reactive, z scatter, w scatter anisotropy (fragment only)
} pc;

layout(location = 0) out vec2 v_uv;        // quad coordinates, [-1, 1]^2 (x right, y up)
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec3 v_world;
layout(location = 3) out vec4 v_misc;      // x age, y random, z frame, w view depth
layout(location = 4) out vec3 v_right;     // the quad's world axes, for lighting
layout(location = 5) out vec3 v_up;
layout(location = 6) out vec3 v_normal;    // toward the viewer

const int MODE_BILLBOARD  = 0;
const int MODE_STRETCHED  = 1;
const int MODE_HORIZONTAL = 2;
const int MODE_VERTICAL   = 3;
const int MODE_ALIGNED    = 4;

vec3 quat_rotate(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
    const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                   vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    vec2 c = corners[gl_VertexIndex];

    vec3  P    = i_pos_size.xyz;
    float size = i_pos_size.w;
    float half_h = 0.5 * size;
    float half_w = half_h * pc.shape.w;
    int   mode = int(pc.mode.x + 0.5);

    // The camera's world-space basis: the rows of the view matrix's rotation.
    vec3 cam_right = vec3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    vec3 cam_up    = vec3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);
    vec3 cam_back  = vec3(camera.view[0][2], camera.view[1][2], camera.view[2][2]);
    vec3 to_cam    = camera.camera_pos - P;

    vec3 right = cam_right;
    vec3 up    = cam_up;
    vec3 nrm   = cam_back;
    bool rotate_in_plane = true;

    if (mode == MODE_STRETCHED) {
        // Long axis along the velocity, the short one across it facing the camera. The quad is
        // as long as its size plus speed * stretch_speed, and trails BEHIND the particle.
        vec3  vel   = i_velocity_rot.xyz;
        float speed = length(vel);
        vec3  dir   = speed > 1e-4 ? vel / speed : cam_up;
        vec3  side  = cross(dir, to_cam);
        float sl    = length(side);
        right = sl > 1e-5 ? side / sl : cam_right;
        up    = dir;
        nrm   = normalize(cross(right, up));
        float len = size * pc.stretch.y + speed * pc.stretch.x;
        half_h = 0.5 * max(len, size * 0.25);
        half_w = 0.5 * size * pc.shape.w * 0.5;
        P -= dir * max(half_h - 0.5 * size, 0.0);
        rotate_in_plane = false;
    } else if (mode == MODE_HORIZONTAL) {
        right = vec3(1.0, 0.0, 0.0);
        up    = vec3(0.0, 1.0, 0.0);
        nrm   = vec3(0.0, 0.0, 1.0);
    } else if (mode == MODE_VERTICAL) {
        // Upright, swung about world Z only: flames and grass cards stay vertical.
        up = vec3(0.0, 0.0, 1.0);
        vec3 r = cam_right - up * dot(cam_right, up);
        float rl = length(r);
        right = rl > 1e-4 ? r / rl : vec3(1.0, 0.0, 0.0);
        nrm = cross(right, up);
        rotate_in_plane = false;
    } else if (mode == MODE_ALIGNED) {
        right = quat_rotate(i_orient, vec3(1.0, 0.0, 0.0));
        up    = quat_rotate(i_orient, vec3(0.0, 1.0, 0.0));
        nrm   = quat_rotate(i_orient, vec3(0.0, 0.0, 1.0));
    }

    if (rotate_in_plane) {
        float s = sin(i_velocity_rot.w);
        float k = cos(i_velocity_rot.w);
        vec3 r2 = right * k + up * s;
        vec3 u2 = up * k - right * s;
        right = r2;
        up = u2;
    }

    // Pivot: shift along the quad's up so e.g. a flame or a grass card stands on its particle.
    P += up * (pc.misc.z * size);

    vec3 world = P + right * (c.x * half_w) + up * (c.y * half_h);
    vec4 view_pos = camera.view * vec4(world, 1.0);
    gl_Position = camera.proj * view_pos;

    // Normals face the viewer, so lighting is consistent from either side of a two-sided quad.
    if (dot(nrm, camera.camera_pos - world) < 0.0) nrm = -nrm;

    v_uv     = c;
    v_color  = i_color;
    v_world  = world;
    v_misc   = vec4(i_misc.xyz, -view_pos.z);
    v_right  = right;
    v_up     = up;
    v_normal = nrm;
}
