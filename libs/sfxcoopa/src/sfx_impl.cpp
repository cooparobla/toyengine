/**
 * @file sfx_impl.cpp
 * @brief The single translation unit that compiles miniaudio's implementation
 * for the entire dependency graph.
 *
 * miniaudio is a single-header library whose implementation block must be
 * compiled in EXACTLY ONE translation unit per final binary. The
 * `sfxcoopa_impl` STATIC target owns it once, and consumers link against
 * `coopa::sfx` (an INTERFACE target depending on `sfxcoopa_impl`) without
 * ever defining MINIAUDIO_IMPLEMENTATION themselves.
 *
 * The MINIAUDIO_IMPLEMENTATION macro itself is supplied via
 * target_compile_definitions() on `sfxcoopa_impl` in CMakeLists.txt, not
 * defined here, so this file stays a pure include. The MA_NO_* trim macros
 * are PUBLIC on `sfxcoopa_impl`, so this TU and every consumer TU see the
 * same set -- mismatched MA_NO_* between TUs would change miniaudio's struct
 * layouts and break the ODR.
 */

#include <miniaudio/miniaudio.h>
