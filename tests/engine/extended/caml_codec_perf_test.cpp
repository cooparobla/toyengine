/**
 * @file caml_codec_perf_test.cpp
 * @brief The .caml codec's key derivation (PBKDF2) runs once at install, not per decode -- a
 *        wall-clock budget, so extended tier (a loaded machine under ctest -j can miss it).
 */

#include <coopa/testing/test.h>

#include <chrono>
#include <string>
#include <vector>

#include <coopa/yaml/document.h>
#include <toyengine/core/caml_codec.h>

#include "engine/support/checks.h"

COOPA_TEST_SUITE("caml_codec_perf");

/** @brief The PBKDF2 step runs once at install: 200 decodes must not cost 200 key derivations. */
COOPA_TEST(key_derivation_is_cached_across_decodes) {
    toy::core::install_caml_codec();   // what the game's main() does before loading anything
    const std::vector<uint8_t> bytes = toy::core::encode_caml_text("a: 1\n");
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) (void)coopa::yaml::decode_bytes(bytes, "bench.caml");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    expect(ms < 250.0, "200 small .caml decodes take under 250 ms (took " + std::to_string(ms) + " ms)");
}
