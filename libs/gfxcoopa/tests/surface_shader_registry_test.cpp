/**
 * @file surface_shader_registry_test.cpp
 * @brief pipeline/surface_shader.h, no device needed: SurfaceShaderRegistry name resolution --
 *        "" is the stock shader, unknown names fail loudly through require(), and duplicate or
 *        empty names are rejected without being appended.
 */
#include <coopa/testing/test.h>

#include <gfxcoopa/pipeline/surface_shader.h>

COOPA_TEST_SUITE("surface_shader_registry");

using coopa::gfx::pipeline::SurfaceShaderDesc;
using coopa::gfx::pipeline::SurfaceShaderDomain;
using coopa::gfx::pipeline::SurfaceShaderRegistry;

COOPA_TEST(resolves_registered_names_and_rejects_unknown_ones) {
    SurfaceShaderRegistry registry;

    // Empty name -- find()/require() treat it as "the stock shader", never registered.
    EXPECT_TRUE(registry.find("") == nullptr);
    registry.require("");  // must not throw

    // An unregistered name is a hard failure via require(), not a silent fall-back.
    ASSERT_THROWS(registry.require("nonexistent"));
    EXPECT_TRUE(registry.find("nonexistent") == nullptr);

    SurfaceShaderDesc water;
    water.name   = "water";
    water.domain = SurfaceShaderDomain::Transparent;
    water.vert   = "water.vert";
    water.frag   = "water.frag";
    registry.add(water);

    const SurfaceShaderDesc* found = registry.find("water");
    ASSERT_TRUE(found != nullptr);
    EXPECT_TRUE(found->vert == "water.vert");
    EXPECT_TRUE(found->domain == SurfaceShaderDomain::Transparent);
    registry.require("water");  // must not throw
    EXPECT_EQ(registry.all().size(), 1u);
}

COOPA_TEST(duplicate_and_empty_names_are_rejected) {
    SurfaceShaderRegistry registry;
    SurfaceShaderDesc water;
    water.name = "water";
    registry.add(water);

    // Duplicate names are rejected -- a silent overwrite would mean two materials
    // referencing the same name draw with different pipelines depending on
    // registration order (see SurfaceShaderRegistry::add()'s own doc).
    SurfaceShaderDesc dup;
    dup.name = "water";
    ASSERT_THROWS(registry.add(dup));
    EXPECT_EQ(registry.all().size(), 1u);  // the failed add() must not have appended anyway

    ASSERT_THROWS(registry.add(SurfaceShaderDesc{}));
    EXPECT_EQ(registry.all().size(), 1u);
}
