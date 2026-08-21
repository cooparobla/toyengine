#version 450

// Inputs matching Vertex struct (location 0: pos, 1: norm, 2: uv, 3: tangent)
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_tangent; // xyz = tangent, w = handedness
layout(location = 4) in mat4 in_model;   // per-instance (locations 4-7)

// Set 0: Camera UBO
layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
    vec3 camera_pos;
} camera;

// Outputs to G-Buffer fragment shader
layout(location = 0) out vec3 frag_world_pos;
layout(location = 1) out vec3 frag_world_normal;
layout(location = 2) out vec2 frag_uv;
layout(location = 3) out mat3 frag_TBN;

void main() {
    vec4 world_pos = in_model * vec4(in_position, 1.0);
    frag_world_pos = world_pos.xyz;

    // normal_matrix used to be CPU-computed (glm::transpose(glm::inverse(world)))
    // and streamed alongside model; now derived here instead, since scenes use
    // non-uniform scale (e.g. Cornell box walls) so mat3(in_model) alone is wrong.
    mat3 norm_mat = transpose(inverse(mat3(in_model)));
    vec3 N = normalize(norm_mat * in_normal);
    vec3 T = normalize(norm_mat * in_tangent.xyz);
    T = normalize(T - dot(T, N) * N);
    vec3 B = cross(N, T) * in_tangent.w;

    frag_world_normal = N;
    frag_uv = in_uv;
    frag_TBN = mat3(T, B, N);

    gl_Position = camera.proj * camera.view * world_pos;
}
