/**
 * @file textured_quad_pass_test.cpp
 * @brief engine/passes/textured_quad_2d_pass.h: the per-TextureView descriptor cache must
 *        evict on unregister_view(), and TextureView -- the cache key -- compares and hashes
 *        by its handle value.
 */
#include <coopa/testing/test.h>

#include "support/gpu_fixture.h"

#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/engine/passes/textured_quad_2d_pass.h>

#include <array>
#include <functional>

COOPA_TEST_SUITE("textured_quad_pass");

using namespace coopa::gfx;

COOPA_TEST(texture_view_keys_compare_and_hash_by_handle_value) {
    EXPECT_FALSE(TextureView::null().valid());
    EXPECT_TRUE(TextureView{0x1234}.valid());
    EXPECT_TRUE(TextureView{0x1234} == TextureView{0x1234});
    EXPECT_TRUE(TextureView{0x1234} != TextureView{0x5678});
    std::hash<TextureView> hasher;
    EXPECT_EQ(hasher(TextureView{42}), hasher(TextureView{42}));
}

COOPA_TEST(unregister_view_evicts_its_cached_descriptor_set) {
    // descriptor_cache_ is keyed by TextureView, which IS the raw VkImageView handle value,
    // and register_view() early-returns on a key hit. Drivers hand a freed handle straight
    // back to the next vkCreateImageView -- so without eviction, a consumer that destroys one
    // texture and uploads another gets a new view comparing EQUAL to the dead one, skips
    // registration, and is drawn through a descriptor set still describing the destroyed
    // image. It renders the old texture, silently. unregister_view() is what breaks that, and
    // this test pins it.
    gfx_test::GpuFixture gpu;

    engine::passes::TexturedQuad2DDesc desc;
    // The test shaders hard-code their triangle and take no vertex input, so an empty layout
    // is correct -- but the pass rejects a zero stride, and a zero push-constant size, out of
    // hand. A pipeline layout may legally declare bindings its shaders never read.
    desc.vertex_stride      = sizeof(float) * 4;
    desc.push_constant_size = sizeof(float) * 4;

    engine::passes::TexturedQuad2DPass pass(*gpu.device, *gpu.allocator, *gpu.cmd_pool, *gpu.render_pass,
                                            gfx_test::k_vert_spv, gfx_test::k_frag_spv, desc);

    const std::array<uint8_t, 4> px = {10, 20, 30, 255};
    auto tex = engine::data::Texture::upload(*gpu.device, *gpu.allocator, *gpu.cmd_pool,
                                             px.data(), 1, 1, Format::RGBA8_Unorm);
    const TextureView view = tex.view_typed();
    const TextureView fallback = pass.fallback_view();
    ASSERT_TRUE(view != fallback);

    pass.register_view(view);
    const pipeline::DescriptorSet* first = &pass.descriptor_set_for(view);
    EXPECT_TRUE(first != &pass.descriptor_set_for(fallback));

    // After eviction the key is gone, so the lookup falls back rather than handing back a
    // set that describes a texture the caller is about to destroy.
    pass.unregister_view(view);
    EXPECT_TRUE(&pass.descriptor_set_for(view) == &pass.descriptor_set_for(fallback));

    // And re-registering the SAME handle value must allocate a fresh set rather than being
    // swallowed by the early-out -- this is the assertion that actually fails if eviction
    // regresses.
    pass.register_view(view);
    const pipeline::DescriptorSet* second = &pass.descriptor_set_for(view);
    EXPECT_TRUE(second != &pass.descriptor_set_for(fallback));
    EXPECT_TRUE(second != first);

    // Unregistering something never registered is a no-op, not a crash.
    pass.unregister_view(TextureView{0xDEADBEEFu});
}
