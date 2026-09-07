#version 450

// Vertex-buffer-less full-NDC quad, shared by every main-camera SDF pass
// (sdf_gbuffer.frag, sdf_forward.frag, sdf_capture.frag): 6 vertices from
// gl_VertexIndex alone (VertexLayout::none() on the pipeline side -- see
// SdfGBufferPass/SdfForwardPass/SdfCapturePass), covering the full [-1,1]^2
// clip rectangle. The object's actual screen-space bounds are enforced by a
// dynamic scissor the CPU sets from SdfRendererGPU::clip_rect BEFORE this
// draw (see PixelRenderPipeline's record_gbuffer_()/record_transparent_())
// -- not by shaping this quad's geometry -- so every fragment shader can
// reconstruct its view ray from gl_FragCoord without also needing to know
// the quad's own extent.
//
// gl_Position.z = 0 (the near plane, always in-range) is deliberate:
// none of the consuming fragment shaders rely on the rasterizer's
// interpolated depth -- they all write gl_FragDepth explicitly from the
// raymarch's actual hit position -- so the incoming z is never read.

layout(push_constant) uniform PC {
    layout(offset = 0) uint renderer_index;
} pc;

layout(location = 0) out flat uint frag_renderer_index;
// The quad's own NDC corner, interpolated -- exactly this fragment's NDC.xy,
// since gl_Position.w is always 1 here (see file doc: linear interpolation
// of an affine function of NDC.xy is exact when w is constant across the
// primitive). This is what lets every consuming fragment shader reconstruct
// its view ray with no gl_FragCoord/viewport-size plumbing at all.
layout(location = 1) out vec2 frag_ndc;

void main() {
    vec2 corners[6] = vec2[](
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0)
    );
    vec2 ndc = corners[gl_VertexIndex];
    gl_Position = vec4(ndc, 0.0, 1.0);
    frag_renderer_index = pc.renderer_index;
    frag_ndc = ndc;
}
