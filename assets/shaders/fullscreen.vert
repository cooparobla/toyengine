#version 450

// Shared fullscreen-triangle generator, reused by every fullscreen pass
// (pixel_lighting, pixel_post, upscale, debug_gradient) -- mirrors blendy's
// idiom of reusing hiz_downsample.vert.spv across multiple post passes
// instead of adding a dedicated .vert file per pass.

layout(location = 0) out vec2 out_uv;

void main() {
    out_uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(out_uv * 2.0 - 1.0, 0.0, 1.0);
}
