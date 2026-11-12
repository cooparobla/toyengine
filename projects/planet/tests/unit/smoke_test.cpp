/**
 * @file smoke_test.cpp
 * @brief The project's module reaches every executable that compiles it.
 *
 * A TOY_MODULE() registers from a static initializer, which a static library would let the
 * linker drop for a translation unit nothing references -- so the project's sources are
 * compiled straight into the game, the editor and these runners (cmake/ToyProject.cmake).
 * If that ever regresses, a planet build loses its components with no error at all, so the
 * cheapest possible test guards it.
 */

#include <coopa/testing/test.h>

#include <string>

#include <toyengine/core/module.h>

COOPA_TEST_SUITE("smoke");

COOPA_TEST(the_planet_module_is_registered) {
    bool found = false;
    for (const toy::core::Module& m : toy::core::modules()) {
        if (m.name == "planet") found = true;
    }
    EXPECT_TRUE(found);
}

COOPA_TEST(the_planet_module_has_an_init_body) {
    for (const toy::core::Module& m : toy::core::modules()) {
        if (m.name == "planet") EXPECT_TRUE(static_cast<bool>(m.on_engine_init));
    }
}
