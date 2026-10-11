#include <toyengine/render/terrain_gpu.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/util/error.h>

namespace toy {
namespace render {

namespace {

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linear_to_srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

} // namespace

uint32_t TextureArray::mip_count(uint32_t size) {
    uint32_t levels = 1;
    while ((size >> levels) > 0) ++levels;
    return levels;
}

std::vector<std::vector<uint8_t>> TextureArray::build_mips(const std::vector<uint8_t>& level0, uint32_t size, bool srgb) {
    std::vector<std::vector<uint8_t>> mips;
    mips.push_back(level0);
    // sRGB colour is averaged in linear light (a 50/50 black-white checker must not darken).
    float lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = srgb ? srgb_to_linear(static_cast<float>(i) / 255.0f) : static_cast<float>(i) / 255.0f;
    uint32_t s = size;
    while (s > 1) {
        const uint32_t d = std::max(1u, s / 2);
        const std::vector<uint8_t>& src = mips.back();
        std::vector<uint8_t> dst(static_cast<size_t>(d) * d * 4);
        for (uint32_t y = 0; y < d; ++y) {
            for (uint32_t x = 0; x < d; ++x) {
                for (uint32_t c = 0; c < 4; ++c) {
                    float sum = 0.0f;
                    for (uint32_t k = 0; k < 4; ++k) {
                        const uint32_t sx = std::min(s - 1, x * 2 + (k & 1u));
                        const uint32_t sy = std::min(s - 1, y * 2 + (k >> 1));
                        const uint8_t v = src[(static_cast<size_t>(sy) * s + sx) * 4 + c];
                        sum += c == 3 ? static_cast<float>(v) / 255.0f : lut[v];
                    }
                    float avg = sum * 0.25f;
                    if (srgb && c != 3) avg = linear_to_srgb(avg);
                    dst[(static_cast<size_t>(y) * d + x) * 4 + c] =
                        static_cast<uint8_t>(std::clamp(avg * 255.0f + 0.5f, 0.0f, 255.0f));
                }
            }
        }
        mips.push_back(std::move(dst));
        s = d;
    }
    return mips;
}

TextureArray::TextureArray(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                           coopa::gfx::command::CommandPool& cmd_pool, uint32_t size, uint32_t count,
                           const std::vector<std::vector<uint8_t>>& layers, bool srgb)
    : device_(device), allocator_(allocator.handle()), size_(std::max(1u, size)), count_(std::max(1u, count)) {
    levels_ = mip_count(size_);
    const VkFormat format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.extent        = {size_, size_, 1u};
    info.mipLevels     = levels_;
    info.arrayLayers   = count_;
    info.format        = format;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    info.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(allocator_, &info, &alloc, &image_, &allocation_, nullptr) != VK_SUCCESS) {
        throw std::runtime_error("[toyengine] TextureArray: vmaCreateImage failed");
    }

    // Every layer's whole chain, packed into one staging buffer, layer-major then mip.
    std::vector<VkBufferImageCopy> regions;
    std::vector<uint8_t> packed;
    for (uint32_t l = 0; l < count_; ++l) {
        std::vector<uint8_t> level0;
        if (l < layers.size() && layers[l].size() == static_cast<size_t>(size_) * size_ * 4) {
            level0 = layers[l];
        } else {
            level0.assign(static_cast<size_t>(size_) * size_ * 4, 255);   // missing: white
        }
        const auto mips = build_mips(level0, size_, srgb);
        for (uint32_t m = 0; m < levels_ && m < mips.size(); ++m) {
            VkBufferImageCopy r{};
            r.bufferOffset = packed.size();
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, l, 1};
            const uint32_t s = std::max(1u, size_ >> m);
            r.imageExtent = {s, s, 1};
            regions.push_back(r);
            packed.insert(packed.end(), mips[m].begin(), mips[m].end());
        }
    }
    coopa::gfx::memory::Buffer staging = coopa::gfx::memory::Buffer::staging(device, allocator, packed.size());
    staging.upload(packed.data(), packed.size());

    cmd_pool.submit_once([&](coopa::gfx::command::CommandBuffer& cmd) {
        VkImageMemoryBarrier b{};
        b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask       = 0;
        b.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image               = image_;
        b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels_, 0, count_};
        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        vkCmdCopyBufferToImage(cmd.handle(), staging.handle(), image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<uint32_t>(regions.size()), regions.data());
        b.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd.handle(), VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
    });

    VkImageViewCreateInfo v{};
    v.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    v.image    = image_;
    v.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    v.format   = format;
    v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels_, 0, count_};
    GFX_VK_CHECK(vkCreateImageView(device_.handle(), &v, nullptr, &view_));

    VkSamplerCreateInfo si{};
    si.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter        = VK_FILTER_LINEAR;
    si.minFilter        = VK_FILTER_LINEAR;
    si.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeW     = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.anisotropyEnable = VK_TRUE;   // Device enables samplerAnisotropy (core/device.h)
    si.maxAnisotropy    = 8.0f;
    si.minLod           = 0.0f;
    si.maxLod           = static_cast<float>(levels_);
    si.borderColor      = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    GFX_VK_CHECK(vkCreateSampler(device_.handle(), &si, nullptr, &sampler_));
}

TextureArray::~TextureArray() {
    if (sampler_ != VK_NULL_HANDLE) vkDestroySampler(device_.handle(), sampler_, nullptr);
    if (view_ != VK_NULL_HANDLE) vkDestroyImageView(device_.handle(), view_, nullptr);
    if (image_ != VK_NULL_HANDLE) vmaDestroyImage(allocator_, image_, allocation_);
}

TerrainGpuRegistry::TerrainGpuRegistry(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                                       coopa::gfx::command::CommandPool& cmd_pool)
    : device_(device) {
    using coopa::gfx::ShaderStage;
    layout_ = std::make_unique<coopa::gfx::pipeline::DescriptorSetLayout>(
        coopa::gfx::pipeline::DescriptorLayoutBuilder()
            .combined_sampler(0, ShaderStage::Fragment)
            .combined_sampler(1, ShaderStage::Fragment)
            .combined_sampler(2, ShaderStage::Fragment)
            .combined_sampler(3, ShaderStage::Fragment)
            .build(device));
    pool_ = std::make_unique<coopa::gfx::pipeline::DescriptorPool>(
        coopa::gfx::pipeline::DescriptorPoolBuilder().add_sets(*layout_, kMaxSets + 1).build(device));

    const uint8_t white[4] = {255, 255, 255, 255};
    white_ = std::make_unique<coopa::gfx::engine::data::Texture>(coopa::gfx::engine::data::Texture::upload(
        device, allocator, cmd_pool, white, 1, 1, coopa::gfx::Format::RGBA8_Unorm, coopa::gfx::SamplerDesc::pixel_art()));
    white_array_ = std::make_unique<TextureArray>(device, allocator, cmd_pool, 1, 1,
                                                  std::vector<std::vector<uint8_t>>{{255, 255, 255, 255}}, true);
    flat_array_ = std::make_unique<TextureArray>(device, allocator, cmd_pool, 1, 1,
                                                 std::vector<std::vector<uint8_t>>{{128, 128, 255, 255}}, false);

    fallback_ = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device, *pool_, *layout_);
    fallback_->bind_image(0, white_->view_typed(), white_->sampler_object());
    fallback_->bind_image(1, white_->view_typed(), white_->sampler_object());
    fallback_->bind_image(2, white_array_->view(), white_array_->sampler());
    fallback_->bind_image(3, flat_array_->view(), flat_array_->sampler());
}

std::unique_ptr<coopa::gfx::pipeline::DescriptorSet> TerrainGpuRegistry::make_set_(const TerrainFrameBinding& b) const {
    auto set = std::make_unique<coopa::gfx::pipeline::DescriptorSet>(device_, *pool_, *layout_);
    const auto* s0 = b.splat0 ? b.splat0.get() : white_.get();
    const auto* s1 = b.splat1 ? b.splat1.get() : white_.get();
    set->bind_image(0, s0->view_typed(), s0->sampler_object());
    set->bind_image(1, s1->view_typed(), s1->sampler_object());
    const TextureArray* albedo = b.albedo ? b.albedo.get() : white_array_.get();
    const TextureArray* normal = b.normal ? b.normal.get() : flat_array_.get();
    set->bind_image(2, albedo->view(), albedo->sampler());
    set->bind_image(3, normal->view(), normal->sampler());
    return set;
}

void TerrainGpuRegistry::update(const std::vector<TerrainFrameBinding>& bindings) {
    released_.clear();
    for (auto it = graveyard_.begin(); it != graveyard_.end();) {
        if (it->frames_left == 0) it = graveyard_.erase(it);
        else { --it->frames_left; ++it; }
    }

    auto same_textures = [](const TerrainFrameBinding& a, const TerrainFrameBinding& b) {
        return a.splat0 == b.splat0 && a.splat1 == b.splat1 && a.albedo == b.albedo && a.normal == b.normal;
    };

    // Retire entries whose key vanished or whose textures were swapped.
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto match = std::find_if(bindings.begin(), bindings.end(),
                                  [&](const TerrainFrameBinding& b) { return b.height == it->first; });
        if (match == bindings.end() || !same_textures(*match, it->second.binding)) {
            if (match == bindings.end()) released_.push_back(it->first);
            graveyard_.push_back({kRetireFrames, std::move(it->second)});
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    for (const TerrainFrameBinding& b : bindings) {
        if (b.height == nullptr || entries_.count(b.height) != 0) continue;
        if (entries_.size() + graveyard_.size() >= kMaxSets) break;   // pool budget
        Entry e;
        e.binding = b;
        e.set = make_set_(b);
        entries_.emplace(b.height, std::move(e));
    }
}

const coopa::gfx::pipeline::DescriptorSet& TerrainGpuRegistry::set_for(const coopa::gfx::engine::data::Texture* height) const {
    auto it = entries_.find(height);
    return it != entries_.end() ? *it->second.set : *fallback_;
}

} // namespace render
} // namespace toy
