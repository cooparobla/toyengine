/**
 * @file gfx_impl.cpp
 * @brief The single translation unit that compiles the vendored implementation
 * blocks (volk, VMA, stb_image, stb_image_write) for the entire dependency
 * graph.
 *
 * volk's loader and VMA's allocator body are single-header libraries whose
 * implementation must be compiled in EXACTLY ONE translation unit per final
 * binary -- defining the macros in a second one is a duplicate-symbol link
 * error. The `gfxcoopa_impl` STATIC target owns that translation unit, and
 * consumers get it transitively by linking `coopa::gfx` (an INTERFACE target
 * depending on `gfxcoopa_impl`) without naming these macros themselves.
 *
 * The macros themselves are supplied via target_compile_definitions() on
 * `gfxcoopa_impl` in CMakeLists.txt, not defined here, so this file stays a
 * pure list of includes.
 */

#include <volk/volk.h>
#include <vma/vk_mem_alloc.h>
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>
