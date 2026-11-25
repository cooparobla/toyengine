/**
 * @file material_maps_test.cpp
 * @brief The material texture path end to end: the material_maps fixture's two scenes -- byte-identical
 *        but for the albedo/normal/metallic-roughness maps -- render measurably differently.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gfxcoopa/engine/components/mesh_renderer.h>
#include <gfxcoopa/engine/data/texture.h>
#include <gfxcoopa/engine/util/material_texture_cache.h>
#include <toyengine/core/engine.h>
#include <toyengine/render/toy_render_pipeline.h>

#include "engine/support/checks.h"
#include "engine/support/render_fixture.h"

COOPA_TEST_SUITE("material_maps");

using namespace toy::test;

/**
 * @brief Renders tests/fixtures/scenes/material_maps's two scenes -- the SAME lit cube,
 * byte-identical YAML but for scene_mapped's three texture_albedo/texture_normal/
 * texture_metallic_roughness keys (see those files' own comments) -- and asserts the frames
 * differ.
 *
 * Exercises the whole path end to end: TextureLoader's sRGB/linear colour-space split,
 * MaterialTextureCache's 4-binding material set, and gbuffer_fs.glsl's albedo/normal/MR
 * sampling. Two Engines here are unavoidable -- the difference lives in two different scene
 * files, and a scene is loaded once at construction -- but FIXED_DT=0 means the two frames are
 * each reproducible, so the comparison can demand a real area of change rather than the single
 * differing byte a "> 0" threshold would accept.
 */
COOPA_TEST(texture_maps_change_the_rendered_frame) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    auto render_scene = [](const char* scene_path) {
        toy::core::AppConfig config = make_test_config(scene_path, 640, 360, 160, 90);
        toy::core::Engine engine(std::move(config));
        tick_frames(engine, 3);
        return engine.capture_image(/*low_res=*/true);
    };

    const Frame flat   = render_scene("src/tests/fixtures/scenes/material_maps/scene_flat.yaml");
    const Frame mapped = render_scene("src/tests/fixtures/scenes/material_maps/scene_mapped.yaml");

    expect(same_extent(flat, mapped), "material maps: both captures share one extent");
    if (!same_extent(flat, mapped)) return;

    const long long diff = count_diff(flat, mapped);
    expect_at_least(diff, 50,
                    "albedo/normal/metallic_roughness maps measurably change the rendered frame");
    if (diff < 50) {
        dump_frame(flat, "material_maps_flat");
        dump_frame(mapped, "material_maps_mapped");
    }
}

/**
 * @brief Re-publishing a material's albedo map far more times than MaterialTextureCache has
 * descriptor sets neither exhausts its pool nor grows the cache without bound.
 *
 * This is the shape every generated world has: a texture built at run time is published under
 * a synthetic asset id, and create() on a live id ALWAYS re-publishes -- retiring the old
 * payload and handing out a new Texture at a new address, which the cache sees as a new
 * combination. Nothing calls release() for an ordinary material texture, so before the cache
 * learnt to evict, the combinations accumulated for the life of the process and the 65th one
 * aborted the program out of vkAllocateDescriptorSets with VK_ERROR_OUT_OF_POOL_MEMORY. Two
 * hundred re-publications is a few minutes of switching between worlds.
 *
 * The cheap 4x4 uploads here are deliberate: what is under test is the descriptor bookkeeping,
 * not the image contents, and the frame between them is what gives the cache a tick() to age
 * entries on.
 */
COOPA_TEST(republished_textures_do_not_exhaust_the_material_pool) {
    ScopedEnv fixed_dt("FIXED_DT", "0");
    ScopedEnv no_input("NO_INPUT", "1");

    using coopa::gfx::engine::data::Texture;
    using coopa::gfx::engine::util::MaterialTextureCache;

    toy::core::AppConfig config =
        make_test_config("src/tests/fixtures/scenes/material_maps/scene_flat.yaml", 320, 180, 160, 90);
    toy::core::Engine engine(std::move(config));
    tick_frames(engine, 2);

    auto* cube = engine.scene().find_object("cube");
    auto* renderer =
        cube ? cube->get_component<coopa::gfx::engine::components::MeshRenderer>() : nullptr;
    expect(renderer != nullptr, "material pool: the fixture cube has a MeshRenderer to texture");
    if (renderer == nullptr) return;

    // One republication per iteration, each one a distinct combination the cache has never
    // seen. Well past kMaxMaterialSets, so a cache that only ever grew would be long dead.
    const int kRepublications = static_cast<int>(MaterialTextureCache::kMaxMaterialSets) + 64;
    std::size_t peak = 0;
    for (int i = 0; i < kRepublications; ++i) {
        const uint8_t pixel = static_cast<uint8_t>(i & 0xFF);
        const std::vector<uint8_t> pixels(4 * 4 * 4, pixel);
        renderer->material.albedo_handle = engine.assets().create<Texture>(
            "runtime/test/material_pool/albedo",
            std::make_shared<Texture>(Texture::upload(engine.device(), engine.allocator(),
                                                      engine.command_pool(), pixels.data(), 4, 4,
                                                      true)));
        tick_frames(engine, 1);
        peak = std::max(peak, engine.pipeline().material_set_count());
    }

    // Within the pool it was sized for -- the assertion the pre-fix cache failed by aborting.
    EXPECT_LE(peak, static_cast<std::size_t>(MaterialTextureCache::kMaxMaterialSets));
    // ...and this run really does reach for more sets than the pool used to hold, so the check
    // above cannot pass by the loop quietly failing to allocate anything.
    EXPECT_GT(peak, std::size_t{64});

    // And the working set collapses back once the stale combinations go idle: kIdleFrames of
    // ticks with one texture bound leaves roughly what this scene actually draws.
    tick_frames(engine, static_cast<int>(MaterialTextureCache::kIdleFrames) + 8);
    EXPECT_LE(engine.pipeline().material_set_count(), std::size_t{8});
}
