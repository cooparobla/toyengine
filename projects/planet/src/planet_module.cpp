/**
 * @file planet_module.cpp
 * @brief The project's entry point into the engine: what a planet build registers.
 *
 * The engine knows nothing about planets, so everything that makes a world round is registered
 * from here -- component parsers, the terrain streaming policy, the atmosphere and camera-fit
 * systems, the surface shaders, and (under TOY_EDITOR) the inspector schemas. The body runs
 * inside every Engine constructor, in the game and in the editor's in-process Play alike.
 *
 * See toyengine/core/module.h for the hook contract.
 */

#include <toyengine/core/module.h>

TOY_MODULE(planet) {
    // Registrations land here as the split proceeds; the module itself exists from the start so
    // its static initializer is proven to survive into every executable that compiles this file.
}
