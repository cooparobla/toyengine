/**
 * @file command_buffer.h
 * @brief High-level command buffer recording wrapper.
 *
 * Wraps a raw VkCommandBuffer with OpenGL-like named methods:
 * begin/end for recording lifetime, begin_render_pass/end_render_pass for
 * render scope, bind_pipeline/bind_vertex_buffer/draw for draw commands.
 *
 * CommandBuffer does NOT own the underlying VkCommandBuffer — it is a
 * non-owning view that wraps a buffer allocated from a CommandPool.
 */

#ifndef COOPA_GFX_COMMAND_COMMAND_BUFFER_H
#define COOPA_GFX_COMMAND_COMMAND_BUFFER_H

#include <volk/volk.h>
#include <vector>
#include <array>
#include <stdexcept>
#include <string>

#include <gfxcoopa/core/device.h>
#include <gfxcoopa/core/swapchain.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/pipeline/pipeline.h>
#include <gfxcoopa/pipeline/compute_pipeline.h>
#include <gfxcoopa/pipeline/descriptor.h>
#include <gfxcoopa/pipeline/render_pass.h>
#include <gfxcoopa/util/error.h>
#include <gfxcoopa/types/enums.h>
#include <gfxcoopa/types/clear.h>
#include <gfxcoopa/detail/vk_convert.h>

namespace coopa {
namespace gfx {
namespace command {

/**
 * @class CommandBuffer
 * @brief Non-owning wrapper around VkCommandBuffer providing named recording methods.
 *
 * Intended to be used as a short-lived object during a frame:
 * @code
 * CommandBuffer cmd(raw_vk_cmd);
 * cmd.begin();
 * cmd.begin_render_pass(render_pass, framebuffer, extent, clear_color);
 * cmd.bind_pipeline(pipeline);
 * cmd.bind_vertex_buffer(vertex_buffer);
 * cmd.set_viewport(0, 0, w, h);
 * cmd.draw(3); // triangle
 * cmd.end_render_pass();
 * cmd.end();
 * @endcode
 */
class CommandBuffer {
public:
    /**
     * @brief Constructs a CommandBuffer view around an existing VkCommandBuffer.
     *
     * The raw command buffer must have been allocated from a CommandPool and
     * must remain valid for the lifetime of this wrapper.
     *
     * @param cmd The raw VkCommandBuffer to wrap.
     */
    explicit CommandBuffer(VkCommandBuffer cmd) : cmd_(cmd) {}

    // --- Recording lifecycle ---

    /**
     * @brief Begins command buffer recording (analogous to starting a GL display list).
     *
     * @param one_time_submit If true, hints that the buffer will be submitted once
     *                        (VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT).
     * @throws std::runtime_error if vkBeginCommandBuffer fails.
     */
    void begin(bool one_time_submit = false) {
        VkCommandBufferBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags = one_time_submit ? VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT : 0;
        GFX_VK_CHECK(vkBeginCommandBuffer(cmd_, &info));
    }

    /**
     * @brief Ends command buffer recording.
     * @throws std::runtime_error if vkEndCommandBuffer fails.
     */
    void end() {
        GFX_VK_CHECK(vkEndCommandBuffer(cmd_));
    }

    // --- Render pass ---

    /**
     * @brief Begins a render pass and sets the clear values.
     *
     * @param render_pass   The render pass to begin.
     * @param framebuffer   The framebuffer to render into.
     * @param extent        Render area (typically the swapchain extent).
     * @param clear_color   RGBA clear color for the color attachment.
     * @param clear_depth   Depth clear value (default 1.0f = far plane).
     * @param clear_stencil Stencil clear value.
     */
    void begin_render_pass(VkRenderPass        render_pass,
                           VkFramebuffer       framebuffer,
                           VkExtent2D          extent,
                           VkClearColorValue   clear_color   = {{0.0f, 0.0f, 0.0f, 1.0f}},
                           float               clear_depth   = 1.0f,
                           uint32_t            clear_stencil = 0)
    {
        std::array<VkClearValue, 2> clear_values{};
        clear_values[0].color        = clear_color;
        clear_values[1].depthStencil = { clear_depth, clear_stencil };

        VkRenderPassBeginInfo rp_info{};
        rp_info.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_info.renderPass        = render_pass;
        rp_info.framebuffer       = framebuffer;
        rp_info.renderArea.offset = {0, 0};
        rp_info.renderArea.extent = extent;
        rp_info.clearValueCount   = static_cast<uint32_t>(clear_values.size());
        rp_info.pClearValues      = clear_values.data();

        vkCmdBeginRenderPass(cmd_, &rp_info, VK_SUBPASS_CONTENTS_INLINE);
    }

    /**
     * @brief Ends the current render pass.
     */
    void end_render_pass() {
        vkCmdEndRenderPass(cmd_);
    }

    // --- Pipeline binding ---

    /**
     * @brief Binds a graphics pipeline (analogous to glUseProgram).
     *
     * Also remembers `p`'s layout, so the sealed bind_descriptor_set(uint32_t,
     * const DescriptorSet&) and push_constants(ShaderStage, ...) overloads
     * below never need a caller-supplied VkPipelineLayout -- eliminating
     * Pipeline::layout() from every call site that isn't gfxcoopa itself.
     *
     * @param pipeline The pipeline to bind.
     */
    void bind_pipeline(const pipeline::Pipeline& p) {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p.handle());
        bound_layout_ = p.layout();
        bound_point_  = VK_PIPELINE_BIND_POINT_GRAPHICS;
    }

    /**
     * @brief Binds a compute pipeline. Like the graphics overload it remembers the layout,
     * and it also switches the sealed bind_descriptor_set()/push_constants() overloads to the
     * compute bind point until the next bind_pipeline(). Graphics and compute bindings are
     * separate in Vulkan, so binding a compute pipeline mid-frame leaves the bound graphics
     * pipeline and its sets in place -- but record compute outside render passes.
     * @param p The compute pipeline to bind.
     */
    void bind_pipeline(const pipeline::ComputePipeline& p) {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, p.handle());
        bound_layout_ = p.layout();
        bound_point_  = VK_PIPELINE_BIND_POINT_COMPUTE;
    }

    /**
     * @brief Binds a vertex buffer to a vertex input binding slot
     * (analogous to glBindVertexArray + glBindBuffer).
     * @param buffer     The vertex buffer to bind.
     * @param offset     Byte offset into the buffer.
     * @param binding    Vertex input binding slot index.
     */
    void bind_vertex_buffer(const memory::Buffer& buffer,
                            VkDeviceSize offset  = 0,
                            uint32_t     binding = 0)
    {
        VkBuffer     raw  = buffer.handle();
        VkDeviceSize off  = offset;
        vkCmdBindVertexBuffers(cmd_, binding, 1, &raw, &off);
    }

    /**
     * @brief Binds an index buffer (analogous to glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ...)).
     * @param buffer    The index buffer to bind.
     * @param index_type VK_INDEX_TYPE_UINT16 or VK_INDEX_TYPE_UINT32.
     * @param offset    Byte offset into the buffer.
     */
    void bind_index_buffer(const memory::Buffer& buffer,
                           VkIndexType index_type = VK_INDEX_TYPE_UINT32,
                           VkDeviceSize offset    = 0)
    {
        vkCmdBindIndexBuffer(cmd_, buffer.handle(), offset, index_type);
    }

    /**
     * @brief Binds an index buffer, using the sealed IndexType instead of
     * VkIndexType. `index_type` has no default (unlike the raw overload
     * above) so that a zero-extra-argument call `bind_index_buffer(buffer)`
     * stays unambiguous, resolving to the raw overload's UINT32 default.
     * @param buffer     The index buffer to bind.
     * @param index_type U16 or U32.
     */
    void bind_index_buffer(const memory::Buffer& buffer, IndexType index_type) {
        bind_index_buffer(buffer, detail::to_vk(index_type), 0);
    }

    /**
     * @brief Binds a descriptor set to the graphics pipeline (analogous to glBindBufferBase/glBindTexture).
     * @param pipeline_layout The pipeline layout that owns the descriptor set layout.
     * @param set             The descriptor set to bind.
     * @param set_index       The descriptor set index (binding point).
     */
    void bind_descriptor_set(VkPipelineLayout              pipeline_layout,
                             const pipeline::DescriptorSet& set,
                             uint32_t                       set_index = 0)
    {
        VkDescriptorSet raw = set.handle();
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline_layout, set_index, 1, &raw, 0, nullptr);
    }

    /**
     * @brief Binds a descriptor set to the graphics pipeline, using the
     * layout of the most recently bound pipeline (see bind_pipeline()) --
     * no caller-supplied VkPipelineLayout needed.
     *
     * `set` is the first parameter (not `set_index`, unlike the raw
     * overload above) deliberately: a literal `0` is a valid null-pointer
     * constant, so it converts to both this overload's `set_index`
     * (uint32_t) and the raw overload's `pipeline_layout` (VkPipelineLayout)
     * equally well, which makes `bind_descriptor_set(0, set)` genuinely
     * ambiguous between the two. Putting `set` (a DescriptorSet&, which
     * cannot convert to VkPipelineLayout at all) first rules the raw
     * overload out as a candidate whenever this one is meant, at the cost
     * of the two overloads' set_index parameter not lining up positionally.
     *
     * @param set       The descriptor set to bind.
     * @param set_index The descriptor set index (binding point).
     * @throws std::runtime_error if no pipeline has been bound yet.
     */
    void bind_descriptor_set(const pipeline::DescriptorSet& set, uint32_t set_index = 0) {
        require_bound_pipeline("bind_descriptor_set");
        VkDescriptorSet raw = set.handle();
        vkCmdBindDescriptorSets(cmd_, bound_point_, bound_layout_, set_index, 1, &raw, 0, nullptr);
    }

    /**
     * @brief Records a push constant update (analogous to setting a uniform directly).
     *
     * @param pipeline_layout The pipeline layout that declared the push constant range.
     * @param stages          Shader stages that read this push constant.
     * @param offset          Byte offset into the push constant block.
     * @param size            Number of bytes to update.
     * @param data            Pointer to the data to upload.
     */
    void push_constants(VkPipelineLayout   pipeline_layout,
                        VkShaderStageFlags stages,
                        uint32_t           offset,
                        uint32_t           size,
                        const void*        data)
    {
        vkCmdPushConstants(cmd_, pipeline_layout, stages, offset, size, data);
    }

    /**
     * @brief Records a push constant update, using the layout of the most
     * recently bound pipeline (see bind_pipeline()) and the sealed
     * ShaderStage instead of a caller-supplied VkPipelineLayout/
     * VkShaderStageFlags pair.
     * @param stages Shader stages that read this push constant.
     * @param offset Byte offset into the push constant block.
     * @param size   Number of bytes to update.
     * @param data   Pointer to the data to upload.
     * @throws std::runtime_error if no pipeline has been bound yet.
     */
    void push_constants(ShaderStage stages, uint32_t offset, uint32_t size, const void* data) {
        require_bound_pipeline("push_constants");
        push_constants(bound_layout_, detail::to_vk(stages), offset, size, data);
    }

    /**
     * @brief Records a push constant update from a typed value, sized and
     * pointer-taken automatically. The common case:
     * `cmd.push_constants(ShaderStage::Fragment, my_push_constants_struct);`
     * @tparam T The push constant struct type (must match the shader's layout).
     * @param stages Shader stages that read this push constant.
     * @param value  The value to upload.
     * @param offset Byte offset into the push constant block.
     */
    template <typename T>
    void push_constants(ShaderStage stages, const T& value, uint32_t offset = 0) {
        push_constants(stages, offset, static_cast<uint32_t>(sizeof(T)), &value);
    }

    // --- Draw calls ---

    /**
     * @brief Records a non-indexed draw call (analogous to glDrawArrays).
     * @param vertex_count Number of vertices to draw.
     * @param first_vertex Index of the first vertex.
     * @param instance_count Number of instances (default 1).
     */
    void draw(uint32_t vertex_count,
              uint32_t first_vertex   = 0,
              uint32_t instance_count = 1)
    {
        vkCmdDraw(cmd_, vertex_count, instance_count, first_vertex, 0);
    }

    /**
     * @brief Records an indexed draw call (analogous to glDrawElements).
     * @param index_count    Number of indices to draw.
     * @param first_index    Offset into the index buffer.
     * @param vertex_offset  Offset added to each index value.
     * @param instance_count Number of instances (default 1).
     * @param first_instance First instance ID; offsets into any bound
     *   VK_VERTEX_INPUT_RATE_INSTANCE stream (gl_InstanceIndex already
     *   includes this — never use gl_InstanceIndex to index anything else).
     *   Affects instance-rate vertex attribute fetching in core Vulkan with
     *   no feature flag required (the drawIndirectFirstInstance device
     *   feature applies only to *indirect* draws, a common confusion).
     */
    void draw_indexed(uint32_t index_count,
                      uint32_t first_index    = 0,
                      int32_t  vertex_offset  = 0,
                      uint32_t instance_count = 1,
                      uint32_t first_instance = 0)
    {
        vkCmdDrawIndexed(cmd_, index_count, instance_count, first_index, vertex_offset, first_instance);
    }

    /**
     * @brief Records a non-indexed draw whose arguments a buffer holds (vkCmdDrawIndirect) --
     * a compute pass writes VkDrawIndirectCommand {vertexCount, instanceCount, firstVertex,
     * firstInstance} (four uint32) and the draw consumes it without a CPU round trip. Put a
     * buffer_barrier(ComputeWrite, Indirect | ...) between the two.
     * @param args       Buffer created with BufferUsage::Indirect.
     * @param offset     Byte offset of the first command (4-byte aligned).
     * @param draw_count Commands to draw; > 1 needs ComputeCaps::multi_draw_indirect.
     * @param stride     Bytes between commands (>= 16).
     */
    void draw_indirect(const memory::Buffer& args, uint64_t offset = 0,
                       uint32_t draw_count = 1, uint32_t stride = 16)
    {
        vkCmdDrawIndirect(cmd_, args.handle(), offset, draw_count, stride);
    }

    /**
     * @brief Indexed sibling of draw_indirect() (vkCmdDrawIndexedIndirect): each command is
     * VkDrawIndexedIndirectCommand {indexCount, instanceCount, firstIndex, vertexOffset,
     * firstInstance} (five 32-bit words).
     */
    void draw_indexed_indirect(const memory::Buffer& args, uint64_t offset = 0,
                               uint32_t draw_count = 1, uint32_t stride = 20)
    {
        vkCmdDrawIndexedIndirect(cmd_, args.handle(), offset, draw_count, stride);
    }

    // --- Compute ---
    //
    // Record outside any render pass, after bind_pipeline(const ComputePipeline&). The
    // command buffer's queue is the graphics queue (Device::supports_compute()).

    /**
     * @brief Dispatches `x * y * z` work groups of the bound compute pipeline
     * (vkCmdDispatch). See ComputePipeline::groups_for() for the usual ceiling divide.
     */
    void dispatch(uint32_t x, uint32_t y = 1, uint32_t z = 1) {
        vkCmdDispatch(cmd_, x, y, z);
    }

    /**
     * @brief Dispatches with group counts read from a buffer (vkCmdDispatchIndirect): three
     * uint32 {x, y, z} at `offset`, typically written by an earlier dispatch. Separate the
     * writer from this with buffer_barrier(ComputeWrite, Indirect | ComputeRead).
     * @param args   Buffer created with BufferUsage::Indirect.
     * @param offset Byte offset of the arguments (4-byte aligned).
     */
    void dispatch_indirect(const memory::Buffer& args, uint64_t offset = 0) {
        vkCmdDispatchIndirect(cmd_, args.handle(), offset);
    }

    /**
     * @brief Fills `size` bytes of a buffer with a repeated 32-bit word (vkCmdFillBuffer) --
     * the cheap way to zero a counter or reset an indirect-args block before a dispatch. A
     * transfer write: follow it with transfer_to_compute_barrier() (or a buffer_barrier from
     * TransferWrite) before a shader reads it. Outside render passes only.
     * @param buffer Destination; needs BufferUsage::TransferDst.
     * @param value  The 32-bit pattern.
     * @param offset Byte offset (4-byte aligned).
     * @param size   Bytes to fill (multiple of 4), or 0 for "to the end of the buffer".
     */
    void fill_buffer(memory::Buffer& buffer, uint32_t value, uint64_t offset = 0, uint64_t size = 0) {
        vkCmdFillBuffer(cmd_, buffer.handle(), offset, size == 0 ? VK_WHOLE_SIZE : size, value);
    }

    // --- Buffer memory barriers ---
    //
    // Global memory barriers (VkMemoryBarrier): they order every buffer written by `from`
    // before every read in `to`, which is what a batch of dispatches feeding a batch of draws
    // wants -- one barrier for all of them, rather than one per buffer. Images keep using
    // transition().

    /**
     * @brief Makes writes of kind `from` visible to accesses of kind `to`. Both sides are
     * bitmasks; each bit implies its pipeline stage (see BufferAccess).
     * @code
     * cmd.dispatch(groups);                                                  // writes vertices
     * cmd.buffer_barrier(BufferAccess::ComputeWrite, BufferAccess::VertexAttribute);
     * mesh.bind(cmd); mesh.draw(cmd);
     * @endcode
     */
    void buffer_barrier(BufferAccess from, BufferAccess to) {
        const detail::BarrierMasks src = detail::barrier_masks_for(from);
        const detail::BarrierMasks dst = detail::barrier_masks_for(to);
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = src.access;
        barrier.dstAccessMask = dst.access;
        vkCmdPipelineBarrier(cmd_, src.stage, dst.stage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    /** @brief Compute wrote vertices, indices or indirect args that the next draws consume. */
    void compute_to_draw_barrier() {
        buffer_barrier(BufferAccess::ComputeWrite,
                       BufferAccess::VertexAttribute | BufferAccess::Index | BufferAccess::Indirect);
    }

    /** @brief One dispatch's writes feed the next dispatch (reads, read-modify-writes, or
     *  dispatch_indirect() arguments). */
    void compute_to_compute_barrier() {
        buffer_barrier(BufferAccess::ComputeWrite,
                       BufferAccess::ComputeRead | BufferAccess::ComputeWrite | BufferAccess::Indirect);
    }

    /** @brief Compute wrote storage buffers that vertex or fragment shaders read. */
    void compute_to_graphics_read_barrier() {
        buffer_barrier(BufferAccess::ComputeWrite,
                       BufferAccess::VertexShaderRead | BufferAccess::FragmentShaderRead);
    }

    /** @brief A copy or fill_buffer() wrote data that the next dispatches read or update. */
    void transfer_to_compute_barrier() {
        buffer_barrier(BufferAccess::TransferWrite, BufferAccess::ComputeRead | BufferAccess::ComputeWrite);
    }

    // --- Dynamic state ---

    /**
     * @brief Sets the viewport dynamically (analogous to glViewport).
     * @param x Left edge.
     * @param y Top edge.
     * @param w Width.
     * @param h Height.
     */
    void set_viewport(float x, float y, float w, float h) {
        pipeline::Pipeline::set_viewport(cmd_, x, y, w, h);
    }

    /**
     * @brief Sets the scissor rectangle dynamically (analogous to glScissor).
     * @param x Left edge.
     * @param y Top edge.
     * @param w Width.
     * @param h Height.
     */
    void set_scissor(int32_t x, int32_t y, uint32_t w, uint32_t h) {
        pipeline::Pipeline::set_scissor(cmd_, x, y, w, h);
    }

    // --- Image state transitions, copies, and blits ---
    //
    // Covers the barrier/copy/blit work a consumer needs for screenshot
    // readback, depth-buffer barriers and texture uploads without naming a
    // Vk* type. handle() stays available for anything not covered here.

    /**
     * @brief Transitions an image to a new TextureUsage, inferring the
     * correct access/pipeline-stage masks (and, via the image's own
     * Format, the correct aspect mask) automatically.
     *
     * There is no "from" parameter: the source usage is read from
     * memory::Image::current_usage() and updated afterward, so the caller
     * cannot describe a transition the image is not actually in.
     *
     * @param image The image to transition. Must be a single-mip,
     *   single-layer image (memory::Image's only supported shape).
     * @param to    The usage to transition into.
     */
    void transition(memory::Image& image, TextureUsage to) {
        TextureUsage from = image.current_usage();
        Format fmt = image.format_typed();
        bool depth = is_depth(fmt);
        detail::BarrierMasks src = detail::barrier_masks_for(from, depth);
        detail::BarrierMasks dst = detail::barrier_masks_for(to, depth);

        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout           = detail::to_vk_layout(from);
        barrier.newLayout           = detail::to_vk_layout(to);
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = image.handle();
        barrier.srcAccessMask       = src.access;
        barrier.dstAccessMask       = dst.access;
        barrier.subresourceRange.aspectMask     = detail::aspect_mask_for(fmt);
        barrier.subresourceRange.baseMipLevel   = 0;
        // Every level together: the Image tracks ONE usage for the whole image, so a partial
        // transition would make that record a lie. Code that stages levels through different
        // layouts (mip generation) records raw barriers and calls mark_transitioned() once done.
        barrier.subresourceRange.levelCount     = image.mip_levels();
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;

        vkCmdPipelineBarrier(cmd_, src.stage, dst.stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        image.mark_transitioned(to);
    }

    /**
     * @brief Copies a byte range between two buffers (vkCmdCopyBuffer).
     * @param src     Source buffer.
     * @param dst     Destination buffer.
     * @param size    Number of bytes to copy.
     * @param src_off Byte offset into src.
     * @param dst_off Byte offset into dst.
     */
    void copy_buffer(const memory::Buffer& src, memory::Buffer& dst,
                     uint64_t size, uint64_t src_off = 0, uint64_t dst_off = 0)
    {
        VkBufferCopy region{};
        region.srcOffset = src_off;
        region.dstOffset = dst_off;
        region.size       = size;
        vkCmdCopyBuffer(cmd_, src.handle(), dst.handle(), 1, &region);
    }

    /**
     * @brief Copies buffer bytes into an image (vkCmdCopyBufferToImage).
     * The image must already be transition()ed to TextureUsage::TransferDst.
     * @param src           Source buffer, tightly packed pixel data.
     * @param dst           Destination image.
     * @param extent        Region size in pixels (from image origin).
     * @param mip           Target mip level.
     * @param layer         Target array layer.
     * @param buffer_offset Byte offset into src where pixel data starts.
     */
    void copy_buffer_to_image(const memory::Buffer& src, memory::Image& dst, Extent2D extent,
                              uint32_t mip = 0, uint32_t layer = 0, uint64_t buffer_offset = 0)
    {
        VkBufferImageCopy region{};
        region.bufferOffset      = buffer_offset;
        region.bufferRowLength   = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask     = detail::aspect_mask_for(dst.format_typed());
        region.imageSubresource.mipLevel       = mip;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyBufferToImage(cmd_, src.handle(), dst.handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    /**
     * @brief Copies a buffer into a sub-rectangle of an image (vkCmdCopyBufferToImage with an
     * offset). The image must already be transition()ed to TextureUsage::TransferDst. The
     * buffer holds the region's texels tightly packed (extent.width per row).
     * @param src    Source buffer.
     * @param dst    Destination image.
     * @param x, y   Region origin in the image, in texels.
     * @param extent Region size in texels.
     */
    void copy_buffer_to_image_region(const memory::Buffer& src, memory::Image& dst, int32_t x, int32_t y,
                                     Extent2D extent, uint32_t mip = 0, uint32_t layer = 0, uint64_t buffer_offset = 0)
    {
        VkBufferImageCopy region{};
        region.bufferOffset      = buffer_offset;
        region.bufferRowLength   = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask     = detail::aspect_mask_for(dst.format_typed());
        region.imageSubresource.mipLevel       = mip;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset = {x, y, 0};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyBufferToImage(cmd_, src.handle(), dst.handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    /**
     * @brief Copies image texels into a buffer (vkCmdCopyImageToBuffer).
     * The image must already be transition()ed to TextureUsage::TransferSrc.
     * See gfxcoopa/util/image_readback.h for the full readback-to-PNG
     * helper built on top of this.
     * @param src    Source image.
     * @param dst    Destination buffer. Must be at least
     *   extent.width * extent.height * format_byte_size(src's format) bytes.
     * @param extent Region size in pixels (from image origin).
     * @param mip    Source mip level.
     * @param layer  Source array layer.
     */
    void copy_image_to_buffer(const memory::Image& src, memory::Buffer& dst, Extent2D extent,
                              uint32_t mip = 0, uint32_t layer = 0)
    {
        VkBufferImageCopy region{};
        region.bufferOffset      = 0;
        region.bufferRowLength   = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask     = detail::aspect_mask_for(src.format_typed());
        region.imageSubresource.mipLevel       = mip;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount     = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cmd_, src.handle(),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.handle(), 1, &region);
    }

    /**
     * @brief Blits (copies with optional scaling/filtering) between two
     * image regions (vkCmdBlitImage). `src` must already be
     * TextureUsage::TransferSrc and `dst` TextureUsage::TransferDst.
     * @param src   Source image.
     * @param dst   Destination image.
     * @param src_r Source region (offset + extent + mip/layer).
     * @param dst_r Destination region.
     * @param filter Nearest or Linear scaling filter.
     */
    void blit(const memory::Image& src, memory::Image& dst,
             ImageRegion src_r, ImageRegion dst_r, Filter filter)
    {
        VkImageBlit region{};
        region.srcSubresource.aspectMask     = detail::aspect_mask_for(src.format_typed());
        region.srcSubresource.mipLevel       = src_r.mip;
        region.srcSubresource.baseArrayLayer = src_r.layer;
        region.srcSubresource.layerCount     = 1;
        region.srcOffsets[0] = { src_r.x, src_r.y, 0 };
        region.srcOffsets[1] = { src_r.x + static_cast<int32_t>(src_r.width),
                                 src_r.y + static_cast<int32_t>(src_r.height), 1 };
        region.dstSubresource.aspectMask     = detail::aspect_mask_for(dst.format_typed());
        region.dstSubresource.mipLevel       = dst_r.mip;
        region.dstSubresource.baseArrayLayer = dst_r.layer;
        region.dstSubresource.layerCount     = 1;
        region.dstOffsets[0] = { dst_r.x, dst_r.y, 0 };
        region.dstOffsets[1] = { dst_r.x + static_cast<int32_t>(dst_r.width),
                                 dst_r.y + static_cast<int32_t>(dst_r.height), 1 };

        vkCmdBlitImage(cmd_, src.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, detail::to_vk(filter));
    }

    /**
     * @brief Clears an image to a solid color (vkCmdClearColorImage).
     * `image` must already be transition()ed to TextureUsage::TransferDst.
     * @param image Image to clear.
     * @param color Clear color.
     */
    void clear_color(memory::Image& image, ClearColor color) {
        VkClearColorValue vk_color = detail::to_vk(color);
        VkImageSubresourceRange range{};
        range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel   = 0;
        range.levelCount     = 1;
        range.baseArrayLayer = 0;
        range.layerCount     = 1;
        vkCmdClearColorImage(cmd_, image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &vk_color, 1, &range);
    }

    /**
     * @brief Returns the raw VkCommandBuffer handle.
     * @return Raw VkCommandBuffer.
     */
    VkCommandBuffer handle() const { return cmd_; }

private:
    /// @brief Throws if bind_pipeline() has not been called yet -- guards
    /// the sealed bind_descriptor_set(uint32_t, ...)/push_constants(ShaderStage, ...)
    /// overloads, which need a bound pipeline's layout.
    void require_bound_pipeline(const char* caller) const {
        if (bound_layout_ == VK_NULL_HANDLE) {
            throw std::runtime_error(std::string("[gfxcoopa] CommandBuffer::") + caller +
                                     ": no pipeline bound (call bind_pipeline() first)");
        }
    }

    VkCommandBuffer cmd_ = VK_NULL_HANDLE; /**< The raw command buffer (not owned). */
    /** Set by bind_pipeline() (either overload); back the sealed overloads above. */
    VkPipelineLayout    bound_layout_ = VK_NULL_HANDLE;
    VkPipelineBindPoint bound_point_  = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

} // namespace command
} // namespace gfx
} // namespace coopa

#endif // COOPA_GFX_COMMAND_COMMAND_BUFFER_H
