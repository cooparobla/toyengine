/**
 * @file legend_test.cpp
 * @brief The README legend and the assets/svg swatches agree with `BiomePalette`.
 *
 * The C++ half of the arrangement tools/gen_legend_svg.py describes: that script makes palette,
 * swatches and README agree, and this proves they still do. Reads files outside the code
 * (README.md, assets/svg/) through ROOT_DIR; nothing is written.
 */

#include <coopa/testing/test.h>

#include <cstdio>
#include <string>
#include <utility>

#include <root_directory.h>

#include <glm/glm.hpp>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("legend");

namespace {

/** @brief Formats a palette colour the way the README legend spells it. */
std::string hex_of(const glm::vec3& color) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
                  static_cast<int>(color.r), static_cast<int>(color.g),
                  static_cast<int>(color.b));
    return std::string(buffer);
}

} // namespace

/**
 * @brief The README legend is documentation of a table in the code, and drifts from it.
 *
 * A legend is only useful if it is true, and nothing else would catch a palette
 * entry changed in `map_config.h` without the README following -- the renders
 * would simply stop matching their own key. So the legend is checked here
 * rather than trusted: every biome row must name a real biome, quote its
 * palette colour exactly, and point at a swatch whose fill is that same colour.
 */
COOPA_TEST(readme_legend_matches_the_palette) {
    const std::string root = ROOT_DIR;
    const std::string readme = read_file(root + "/README.md");
    ASSERT_TRUE(!readme.empty());

    const BiomePalette palette;
    int rows_checked = 0;

    for (std::size_t i = 0; i < k_biome_count; ++i) {
        const Biome biome = static_cast<Biome>(i);
        const std::string slug(biome_name(biome));
        const std::string hex = hex_of(palette.color_for(biome));

        // The exact row the legend generator emits, swatch included. Matching the
        // whole row at once is what ties the three columns together: a swatch
        // pointing at the wrong biome, or a hex that disagrees with its own RGB,
        // both fail here rather than passing three separate looser checks.
        const glm::vec3& color = palette.color_for(biome);
        const std::string row = "| ![](assets/svg/" + slug + ".svg) | ";
        const std::string tail = " | `" + slug + "` | `" + hex + "` | "
                               + std::to_string(static_cast<int>(color.r)) + ", "
                               + std::to_string(static_cast<int>(color.g)) + ", "
                               + std::to_string(static_cast<int>(color.b)) + " |";

        const std::size_t at = readme.find(row);
        ASSERT_TRUE(at != std::string::npos);
        ASSERT_TRUE(readme.find(tail, at) != std::string::npos);
        ASSERT_TRUE(readme.find(tail, at) < readme.find('\n', at));

        // And the swatch itself is that colour, not merely a file of the right name.
        const std::string swatch = read_file(root + "/assets/svg/" + slug + ".svg");
        ASSERT_TRUE(!swatch.empty());
        ASSERT_TRUE(swatch.find("fill=\"" + hex + "\"") != std::string::npos);
        ++rows_checked;
    }
    ASSERT_EQ(static_cast<std::size_t>(rows_checked), k_biome_count);

    // The overlay half of the legend, keyed by the swatch each row points at.
    const std::pair<const char*, glm::vec3> overlays[] = {
        {"river", palette.river_color},
        {"bridge", palette.bridge_color},
        {"trail", palette.trail_color},
        {"road", palette.road_color},
        {"highway", palette.highway_color},
        {"building", palette.building_color},
        {"settlement", palette.town_color},
        {"landmark-natural", palette.landmark_natural_color},
        {"landmark-built", palette.landmark_built_color},
        {"background", palette.background_color},
        {"cave-shallow", palette.cave_shallow_color},
        {"cave-deep", palette.cave_deep_color},
        {"cave-chamber", palette.cave_chamber_color},
        {"cave-mouth", palette.cave_mouth_color},
    };
    for (const auto& overlay : overlays) {
        const std::string hex = hex_of(overlay.second);
        const std::string row = "| ![](assets/svg/" + std::string(overlay.first) + ".svg) |";
        const std::size_t at = readme.find(row);
        ASSERT_TRUE(at != std::string::npos);
        ASSERT_TRUE(readme.find("`" + hex + "`", at) < readme.find('\n', at));

        const std::string swatch = read_file(root + "/assets/svg/" + overlay.first + ".svg");
        ASSERT_TRUE(!swatch.empty());
        ASSERT_TRUE(swatch.find("fill=\"" + hex + "\"") != std::string::npos);
    }

    // The tint caveat is the one thing a reader can check against a render and
    // find false, so it may not quietly disappear either.
    ASSERT_TRUE(readme.find("untinted") != std::string::npos);
}
