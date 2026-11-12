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

namespace {

/** @brief The sampler a layer atlas is read through: trilinear, anisotropic, tiling. */
coopa::gfx::SamplerDesc layer_sampler_desc() {
    coopa::gfx::SamplerDesc d;
    d.min = coopa::gfx::Filter::Linear;
    d.mag = coopa::gfx::Filter::Linear;
    d.mipmap = coopa::gfx::MipmapMode::Linear;
    d.address = coopa::gfx::AddressMode::Repeat;
    d.max_anisotropy = 8.0f;   // Device enables samplerAnisotropy (core/device.h)
    d.mipmapped = true;
    return d;
}

} // namespace

TextureArray::TextureArray(coopa::gfx::core::Device& device, coopa::gfx::memory::Allocator& allocator,
                           coopa::gfx::command::CommandPool& cmd_pool, uint32_t size, uint32_t count,
                           const std::vector<std::vector<uint8_t>>& layers, bool srgb)
    : size_(std::max(1u, size)), count_(std::max(1u, count)), levels_(mip_count(std::max(1u, size))),
      image_(device, allocator,
             coopa::gfx::ImageDesc::array_2d(std::max(1u, size), std::max(1u, count),
                                             srgb ? coopa::gfx::Format::RGBA8_Srgb
                                                  : coopa::gfx::Format::RGBA8_Unorm,
                                             coopa::gfx::ImageUsage::TransferDst
                                                 | coopa::gfx::ImageUsage::Sampled,
                                             mip_count(std::max(1u, size)))),
      sampler_(device, layer_sampler_desc(), static_cast<float>(mip_count(std::max(1u, size)))) {
    // Every layer's whole chain, packed into one staging buffer, layer-major then mip.
    std::vector<coopa::gfx::BufferImageRegion> regions;
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
            coopa::gfx::BufferImageRegion r;
            r.buffer_offset = packed.size();
            r.mip = m;
            r.layer = l;
            const uint32_t s = std::max(1u, size_ >> m);
            r.width = s;
            r.height = s;
            regions.push_back(r);
            packed.insert(packed.end(), mips[m].begin(), mips[m].end());
        }
    }
    coopa::gfx::memory::Buffer staging = coopa::gfx::memory::Buffer::staging(device, allocator, packed.size());
    staging.upload(packed.data(), packed.size());

    const coopa::gfx::SubresourceRange whole{0, levels_, 0, count_};
    cmd_pool.submit_once([&](coopa::gfx::command::CommandBuffer& cmd) {
        coopa::gfx::ImageBarrier to_dst;
        to_dst.from = coopa::gfx::TextureUsage::Undefined;
        to_dst.to = coopa::gfx::TextureUsage::TransferDst;
        to_dst.range = whole;
        cmd.barrier(image_, to_dst);

        cmd.copy_buffer_to_image(staging, image_.handle_typed(), regions);

        coopa::gfx::ImageBarrier to_read;
        to_read.from = coopa::gfx::TextureUsage::TransferDst;
        to_read.to = coopa::gfx::TextureUsage::ShaderRead;
        to_read.range = whole;
        // Explicit, not derived: tessellated displacement samples the splat data in the VERTEX
        // stage as well, and ShaderRead's derived mask covers the fragment stage only -- which
        // would leave the vertex read unsynchronised, and correct on most frames.
        to_read.make_visible_to = coopa::gfx::ImageAccess::VertexShaderRead
                                | coopa::gfx::ImageAccess::FragmentShaderRead;
        cmd.barrier(image_, to_read);
    });
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
