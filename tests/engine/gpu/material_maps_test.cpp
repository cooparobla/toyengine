/**
 * @file material_maps_test.cpp
 * @brief The material texture path end to end: the material_maps fixture's two scenes -- byte-identical
 *        but for the albedo/normal/metallic-roughness maps -- render measurably differently.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <toyengine/core/engine.h>

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

    const Frame flat   = render_scene("tests/fixtures/scenes/material_maps/scene_flat.yaml");
    const Frame mapped = render_scene("tests/fixtures/scenes/material_maps/scene_mapped.yaml");

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
