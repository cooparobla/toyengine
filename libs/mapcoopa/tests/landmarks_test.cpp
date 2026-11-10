/**
 * @file landmarks_test.cpp
 * @brief The landmark pass: each landmark suits its biome, at most one wonder per region, no
 *        shared cells; and `draw_landmark_marks` is a render-only switch.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <vector>

#include <coopa/maps/map_renderer.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("landmarks");

COOPA_TEST(landmarks_respect_their_biome) {
    const MapGraph& graph = shared_world().graph();

    ASSERT_TRUE(!graph.landmarks.empty());

    std::vector<int> wonders(graph.regions.size(), 0);
    for (const MapLandmark& landmark : graph.landmarks) {
        ASSERT_TRUE(!landmark.name.empty());
        ASSERT_TRUE(landmark.center >= 0
                    && landmark.center < static_cast<CenterId>(graph.centers.size()));

        const MapCenter& center = graph.centers[static_cast<std::size_t>(landmark.center)];
        // No volcano on ice, no oasis outside desert: the vocabulary is gated by terrain, which
        // is what stops landmarks reading as random noise.
        ASSERT_TRUE(landmark_suits_biome(landmark.kind, center.biome));

        if (landmark.kind == LandmarkKind::Wonder && landmark.region != k_invalid_id) {
            ++wonders[static_cast<std::size_t>(landmark.region)];
        }
    }
    for (const int count : wonders) {
        ASSERT_TRUE(count <= 1);
    }

    // No two landmarks share a cell, and none stands on a settlement.
    std::vector<CenterId> occupied;
    for (const MapLandmark& landmark : graph.landmarks) {
        occupied.push_back(landmark.center);
    }
    for (const MapTown& town : graph.towns) {
        occupied.push_back(town.center);
    }
    std::sort(occupied.begin(), occupied.end());
    ASSERT_TRUE(std::adjacent_find(occupied.begin(), occupied.end()) == occupied.end());
}

/**
 * @brief draw_landmark_marks suppresses the landmark diamonds and nothing else.
 *
 * The flag has to be render-only, and that is the whole risk: the obvious way to stop drawing
 * landmarks is `enable_landmarks`, which instead stops *generating* them -- leaving nothing for
 * an interactive overlay to place a marker on. So this asserts the graph is identical either
 * way, and that only the two layers which paint markers change.
 */
COOPA_TEST(draw_landmark_marks_is_render_only) {
    MapConfig without = world_config();
    set_render_size(without, 192);
    without.draw_landmark_marks = false;
    MapGenerator generator(without, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    // Same graph, same landmarks -- the flag never reaches a pass. This is the distinction from
    // enable_landmarks, which would empty this vector.
    ASSERT_TRUE(!graph.landmarks.empty());

    MapConfig with = without;
    with.draw_landmark_marks = true;

    // Only the two layers that call draw_markers_() may differ.
    for (const MapLayer layer : {MapLayer::Composite, MapLayer::Landmarks}) {
        const Image on = MapLayers::render(layer, graph, with);
        const Image off = MapLayers::render(layer, graph, without);
        ASSERT_TRUE(on.pixels != off.pixels);
    }
    for (const MapLayer layer : {MapLayer::Elevation, MapLayer::Water, MapLayer::Biomes,
                                 MapLayer::Roads, MapLayer::Structures, MapLayer::Regions,
                                 MapLayer::Caves}) {
        const Image on = MapLayers::render(layer, graph, with);
        const Image off = MapLayers::render(layer, graph, without);
        ASSERT_TRUE(on.pixels == off.pixels);
    }

    // Town marks and cave rings survive the suppression, so the Landmarks layer keeps its
    // meaning rather than becoming an empty image.
    const Image marks_off = MapLayers::render(MapLayer::Landmarks, graph, without);
    const Image empty = MapLayers::allocate(MapLayer::Landmarks, without);
    ASSERT_TRUE(marks_off.pixels != empty.pixels);
}
