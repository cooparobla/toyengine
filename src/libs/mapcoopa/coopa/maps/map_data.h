/**
 * @file map_data.h
 * @brief The dual Delaunay/Voronoi graph a generated map is made of: cells,
 *        corners, edges, settlements, and the container that owns them.
 */

#ifndef COOPA_MAPS_MAP_DATA_H
#define COOPA_MAPS_MAP_DATA_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <coopa/maps/biome.h>
#include <coopa/maps/building.h>
#include <coopa/maps/cave.h>
#include <coopa/maps/landmark.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/noise.h>
#include <coopa/maps/portable_sort.h>

namespace coopa {
namespace maps {

/** @brief Index of a `MapCenter` within `MapGraph::centers`. */
using CenterId = std::int32_t;
/** @brief Index of a `MapCorner` within `MapGraph::corners`. */
using CornerId = std::int32_t;
/** @brief Index of a `MapEdge` within `MapGraph::edges`. */
using EdgeId = std::int32_t;
/** @brief Index of a `MapRegion` within `MapGraph::regions`. */
using RegionId = std::int32_t;
/** @brief Index of a `MapCountry` within `MapGraph::countries`. */
using CountryId = std::int32_t;

/** @brief The value an unset `CenterId`, `CornerId` or `EdgeId` holds. */
inline constexpr std::int32_t k_invalid_id = -1;

/**
 * @struct MapPoint
 * @brief A position in grid space, where the map spans `[0, grid_size]` on both axes.
 */
struct MapPoint {
    double x = 0.0; /**< @brief Horizontal position in grid units. */
    double y = 0.0; /**< @brief Vertical position in grid units. */
    /**
     * @brief Third coordinate, used only by sphere maps (`MapTopology::Sphere`), whose points lie
     *        on a globe centred at the origin. Always 0 on a plane, where every formula below
     *        reduces exactly to its 2D form.
     */
    double z = 0.0;

    /**
     * @brief Euclidean distance to another point -- on a sphere the chord, which at the scales
     *        any pass measures is the arc to well under a percent.
     * @param other The point to measure to.
     * @return The distance in grid units.
     */
    double distance_to(const MapPoint& other) const {
        return std::sqrt((x - other.x) * (x - other.x) + (y - other.y) * (y - other.y)
                         + (z - other.z) * (z - other.z));
    }
};

/**
 * @brief Sorts points counter-clockwise about their centroid, in place.
 *
 * Recovers a winding order from an unordered vertex set. Correct only for a
 * convex polygon -- a subdivided cell boundary is not one, which is why
 * `MapGraph::cell_outline()` builds its result in order and falls back to this
 * only when a cell's edges do not form a closed ring.
 *
 * @param points The points to order.
 */
inline void sort_points_radially(std::vector<MapPoint>& points) {
    if (points.size() < 3) {
        return;
    }
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (const MapPoint& point : points) {
        sum_x += point.x;
        sum_y += point.y;
    }
    const double count = static_cast<double>(points.size());
    const MapPoint centroid{sum_x / count, sum_y / count};

    coopa::maps::sort(points.begin(), points.end(), [centroid](const MapPoint& a, const MapPoint& b) {
        return std::atan2(a.y - centroid.y, a.x - centroid.x)
             < std::atan2(b.y - centroid.y, b.x - centroid.x);
    });
}

/**
 * @brief Ray-casting point-in-polygon test; an odd crossing count means inside.
 *
 * Works for any simple polygon, convex or not, which matters because a cell
 * outline is convex only before its edges are subdivided.
 *
 * @param polygon The polygon's vertices, in winding order.
 * @param point The point to test.
 * @return True if the point lies inside the polygon.
 */
inline bool point_in_polygon(const std::vector<MapPoint>& polygon, const MapPoint& point) {
    bool inside = false;
    const std::size_t count = polygon.size();
    if (count < 3) {
        return false;
    }
    for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
        const MapPoint& a = polygon[i];
        const MapPoint& b = polygon[j];
        if ((a.y > point.y) != (b.y > point.y)) {
            const double denominator = b.y - a.y;
            if (denominator == 0.0) {
                continue;
            }
            const double x_at_y = (b.x - a.x) * (point.y - a.y) / denominator + a.x;
            if (point.x < x_at_y) {
                inside = !inside;
            }
        }
    }
    return inside;
}

/**
 * @brief Chaikin corner-cutting, with the two endpoints pinned.
 *
 * Each interior segment is replaced by its quarter and three-quarter points,
 * which rounds every corner without the curve drifting off the points it was
 * derived from. The ends are kept exactly where they are so a smoothed path
 * still meets the junction, the settlement or the coastline it was traced to --
 * a road that stops a quarter of a cell short of its own junction is worse than
 * one drawn straight.
 *
 * Shared by the road and river passes. Both trace a chain of graph positions and
 * both need the joints rounded off without the course being straightened: the
 * meander of a river and the switchback of a mountain road are the *data*, and
 * corner-cutting preserves them precisely because it never moves a point far.
 *
 * @param points The path to smooth, in place.
 * @param iterations Passes to apply; zero or fewer leaves the path alone.
 */
inline void chaikin_smooth(std::vector<MapPoint>& points, int iterations) {
    for (int pass = 0; pass < iterations && points.size() > 2; ++pass) {
        std::vector<MapPoint> cut;
        cut.reserve(points.size() * 2);
        cut.push_back(points.front());
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            const MapPoint& a = points[i];
            const MapPoint& b = points[i + 1];
            cut.push_back({a.x * 0.75 + b.x * 0.25, a.y * 0.75 + b.y * 0.25, a.z * 0.75 + b.z * 0.25});
            cut.push_back({a.x * 0.25 + b.x * 0.75, a.y * 0.25 + b.y * 0.75, a.z * 0.25 + b.z * 0.75});
        }
        cut.push_back(points.back());
        points = std::move(cut);
    }
}

/**
 * @struct MapCenter
 * @brief One Voronoi cell -- a polygon of terrain, and a vertex of the Delaunay triangulation.
 *
 * This is the unit gameplay cares about: it carries the biome, the elevation
 * and the moisture. Its three adjacency lists hold indices into `MapGraph`
 * rather than pointers: `shared_ptr`s in both directions would make
 * `center -> corners -> touches -> center` a reference cycle and leak the
 * entire graph on every generation.
 */
struct MapCenter {
    CenterId index = k_invalid_id; /**< @brief This cell's own index; equals its slot in `MapGraph::centers`. */
    MapPoint point;                /**< @brief The generating site, at the polygon's approximate centre. */

    bool water = false;  /**< @brief Lake or ocean. */
    bool ocean = false;  /**< @brief Water connected to the map border; false for an inland lake. */
    bool coast = false;  /**< @brief Land bordering at least one ocean cell. */
    bool border = false; /**< @brief Lies in the forced-water band at the edge of the map. */

    Biome biome = Biome::Ocean; /**< @brief Terrain classification, assigned by the biome pass. */
    double elevation = 0.0;     /**< @brief Mean of the cell's corner elevations, in `[0, 1]`. */
    /**
     * @brief Height of the water surface over this cell; meaningless on dry land.
     *
     * Flat per *body* of water, which `elevation` is not. The sea sits at
     * `MapConfig::sea_level` everywhere, and every cell of one lake shares that
     * lake's single surface -- so a consumer can flood a terrain mesh to this
     * number directly, and the water layer can draw a sheet rather than a
     * mottled field.
     *
     * `elevation` remains the height of the *ground* underneath, which for a
     * water cell is its bed.
     */
    double water_level = 0.0;
    double moisture = 0.0;      /**< @brief Mean of the cell's corner moistures, in `[0, 1]`. */
    /** @brief Mean of the cell's corner temperatures, in `[0, 1]`; 0 polar, 1 equatorial. */
    double temperature = 0.0;

    /** @brief The region this cell belongs to, or `k_invalid_id` for water and unclaimed land. */
    RegionId region = k_invalid_id;
    /** @brief The country this cell belongs to, or `k_invalid_id`. */
    CountryId country = k_invalid_id;

    std::vector<CenterId> neighbors; /**< @brief Cells sharing an edge with this one. */
    std::vector<EdgeId> borders;     /**< @brief Edges bounding this cell. */
    std::vector<CornerId> corners;   /**< @brief Polygon vertices, sorted counter-clockwise. */
};

/**
 * @struct MapCorner
 * @brief One Voronoi vertex -- the circumcentre of a Delaunay triangle.
 *
 * Elevation, moisture and rivers are all computed here first and averaged down
 * to cells afterwards, because water flows between corners, not between cells.
 */
struct MapCorner {
    CornerId index = k_invalid_id; /**< @brief This corner's own index; equals its slot in `MapGraph::corners`. */
    MapPoint point;                /**< @brief The circumcentre position. */

    bool ocean = false;  /**< @brief Every cell touching this corner is ocean. */
    bool water = false;  /**< @brief Lake or ocean. */
    bool coast = false;  /**< @brief Touches both land and ocean cells. */
    bool border = false; /**< @brief Lies at or beyond the map edge. */

    double elevation = 0.0;    /**< @brief Height in `[0, 1]`, zero at the coast. */
    double moisture = 0.0;     /**< @brief Wetness in `[0, 1]`. */
    double temperature = 0.0;  /**< @brief Warmth in `[0, 1]`; 0 polar, 1 equatorial. */
    int river = 0;             /**< @brief Volume of river water passing through, or 0 for none. */

    std::vector<CenterId> touches;  /**< @brief Cells this corner is a vertex of. */
    std::vector<EdgeId> protrudes;  /**< @brief Edges meeting at this corner. */
    std::vector<CornerId> adjacent; /**< @brief Corners one edge away. */

    /** @brief The adjacent corner water flows to; self when this is a local minimum. */
    CornerId downslope = k_invalid_id;
};

/**
 * @struct MapEdge
 * @brief A Delaunay edge and its dual Voronoi edge, stored as one object.
 *
 * `d0`/`d1` are the two cells the edge separates; `v0`/`v1` are the two corners
 * it runs between. Rivers follow the Voronoi edge, roads follow the Delaunay
 * edge, which is why both live here.
 */
struct MapEdge {
    EdgeId index = k_invalid_id; /**< @brief This edge's own index; equals its slot in `MapGraph::edges`. */

    CenterId d0 = k_invalid_id; /**< @brief Cell on one side. */
    CenterId d1 = k_invalid_id; /**< @brief Cell on the other side. */
    CornerId v0 = k_invalid_id; /**< @brief Corner at one end. */
    CornerId v1 = k_invalid_id; /**< @brief Corner at the other end. */

    MapPoint midpoint; /**< @brief Halfway between `v0` and `v1`. */
    int river = 0;     /**< @brief Volume of water flowing along this edge, or 0. */
    bool noisy = false;/**< @brief The noisy-edge pass has already processed this edge. */
    /**
     * @brief A road runs along this edge.
     *
     * Kept as a plain predicate beside `road_class` because most consumers only
     * ask whether one is there -- the town packer turns a road-flagged border
     * into a street without caring how busy it is. Always equal to
     * `road_class != RoadClass::None`; the road pass is what keeps them agreeing.
     */
    bool road = false;
    /** @brief Traffic tier of the road here; `None` when no road runs along this edge. */
    RoadClass road_class = RoadClass::None;
    /**
     * @brief Routes the network pass sent along this edge; what `road_class` is derived from.
     *
     * Kept rather than discarded after classification so a consumer can re-cut
     * the tiers at its own thresholds, and so a test can check that a highway
     * really did earn the title.
     */
    int traffic = 0;
    /**
     * @brief The road here spans water -- a river, a lake neck or a strait.
     *
     * Exactly representable because a road follows the Delaunay edge `d0`-`d1`
     * and a river the dual Voronoi edge `v0`-`v1`, and those are this same
     * object: the two cross each other by construction, so a road on a
     * river-carrying edge crosses that river and nothing else needs deciding.
     */
    bool bridge = false;

    /** @brief The wobbled path from `v0` to `midpoint`; exactly two points when not subdivided. */
    std::vector<MapPoint> noisy_points0;
    /** @brief The wobbled path from `v1` to `midpoint`; exactly two points when not subdivided. */
    std::vector<MapPoint> noisy_points1;
};

/**
 * @struct MapRoad
 * @brief One continuous run of road, as a smoothed centreline in grid units.
 *
 * The per-edge flags say *where* roads are; this says what one *looks like*.
 * A run is a maximal chain of same-class road edges between two junctions, so a
 * consumer can follow a highway from end to end -- to drive a caravan along it,
 * or to stroke it as a single polyline -- without rediscovering the chain from
 * the edge flags every time.
 *
 * `points` is denser than `edges` is long: the chain of cell sites is corner-cut
 * before it is stored, because a route drawn straight between sites is visibly
 * faceted at every cell and no road is.
 */
struct MapRoad {
    RoadClass road_class = RoadClass::Trail; /**< @brief The class every edge in this run shares. */
    std::vector<MapPoint> points; /**< @brief The smoothed centreline, in grid units, in order. */
    std::vector<EdgeId> edges;    /**< @brief The edges this run covers, in order along it. */
};

/**
 * @struct MapRiver
 * @brief One watercourse, as a smoothed centreline in grid units.
 *
 * The counterpart of `MapRoad`: the per-corner and per-edge `river` volumes say
 * *where* the water is, this says what the channel looks like. Drawn straight
 * between Voronoi corners a river is visibly angular at every corner, which is
 * the one shape moving water never has.
 *
 * Ordered source to mouth, so `volume` -- which is the volume where it ends --
 * is also the largest the channel ever gets.
 */
struct MapRiver {
    std::vector<MapPoint> points;  /**< @brief The smoothed centreline, in grid units, source first. */
    std::vector<CornerId> corners; /**< @brief The corners it runs through, in order. */
    int volume = 0;                /**< @brief Volume at the mouth; the channel widens toward it. */
};

/**
 * @struct MapBuilding
 * @brief One axis-aligned building footprint packed inside a settlement's cell.
 */
struct MapBuilding {
    MapPoint point;        /**< @brief Footprint centre, in grid units. */
    double width = 0.0;    /**< @brief Footprint width, in grid units. */
    double height = 0.0;   /**< @brief Footprint height, in grid units. */
    double rotation = 0.0; /**< @brief Yaw in radians, for a consumer that renders oriented meshes. */
    /**
     * @brief What the building is for; `Dwelling` unless it is part of the civic core.
     *
     * Defaulted, so a reader that ignores roles -- and a map saved without them --
     * sees every building as a dwelling.
     */
    BuildingRole role = BuildingRole::Dwelling;
};

/**
 * @struct MapStreet
 * @brief A lane inside a settlement, which its buildings front onto.
 *
 * The counterpart of `MapRoad`: a road joins settlements, a street is the inside
 * of one. Each runs from a claimed cell's site outward to where it leaves the
 * cell -- at a road entering across a Delaunay edge, at a river bank, or, failing
 * both, toward the cell's farthest corners so that even an isolated hamlet has a
 * lane rather than a scatter.
 *
 * Streets are map data rather than private to the town pass because buildings are
 * lined up along them: a settlement whose streets nobody can see reads as random.
 * Emitted here, a street is drawn, exported, and available to a consumer laying
 * cobbles.
 */
struct MapStreet {
    MapPoint from;        /**< @brief Inner end, at the cell's site. */
    MapPoint to;          /**< @brief Outer end, where the street leaves the cell. */
    double bearing = 0.0; /**< @brief Direction from `from` to `to`, in radians. */
    double length = 0.0;  /**< @brief Distance between the ends, in grid units. */
    /**
     * @brief Extra setback before the first plot, in grid units.
     *
     * A river's half-width, so plots line the bank rather than the channel. Zero
     * for a road, which buildings may front directly.
     */
    double clearance = 0.0;
};

/**
 * @brief Builds a street between two points, deriving its bearing and length.
 *
 * The one place those two are computed. They are derived rather than stored on
 * disk for the same reason: a bearing saved beside its endpoints is a number that
 * can come back disagreeing with them.
 *
 * @param from Inner end, at the cell's site.
 * @param to Outer end, where the street leaves the cell.
 * @param clearance Extra setback before the first plot, in grid units.
 * @return The street, ready to use.
 */
inline MapStreet make_street(const MapPoint& from, const MapPoint& to, double clearance = 0.0) {
    MapStreet street;
    street.from = from;
    street.to = to;
    street.bearing = std::atan2(to.y - from.y, to.x - from.x);
    street.length = from.distance_to(to);
    street.clearance = clearance;
    return street;
}

/**
 * @struct MapPlaza
 * @brief The open ground at the heart of a settlement, kept clear of building.
 *
 * A disc rather than a polygon, deliberately: a disc is one comparison to test a
 * footprint against, one circle to draw and two numbers to serialise, where a
 * polygon would need clipping against the cell outline for nothing anyone can
 * see. `radius` of zero means the settlement has no square -- which is most of
 * them, because a village that could spare the ground for one would be a town.
 */
struct MapPlaza {
    MapPoint centre;      /**< @brief Middle of the open ground, in grid units. */
    double radius = 0.0;  /**< @brief Radius in grid units; 0 when there is no square. */
};

/**
 * @struct MapTown
 * @brief A settlement occupying one cell, with the buildings packed into it.
 */
struct MapTown {
    /**
     * @brief The cell the settlement grew from; the first entry of `cells`.
     *
     * Still the settlement's identity -- its position, its region and its
     * spacing against other settlements are all measured from here.
     */
    CenterId center = k_invalid_id;
    /**
     * @brief Every cell this settlement covers, the primary one first.
     *
     * A settlement larger than a hamlet does not fit in one Voronoi cell, so a
     * capital claims its neighbours and builds across all of them. Disjoint
     * between settlements: a cell belongs to at most one.
     */
    std::vector<CenterId> cells;
    MapPoint point;                  /**< @brief Settlement centre, in grid units. */
    TownTier tier = TownTier::Village; /**< @brief Size class, by site quality rank. */
    double score = 0.0;              /**< @brief Habitability score the site was chosen on. */
    std::string name;                /**< @brief Generated in the dialect of its region. */
    RegionId region = k_invalid_id;  /**< @brief The region this settlement belongs to. */

    /** @brief Number of occupied dwellings; equals `buildings.size()`. */
    int households = 0;
    /**
     * @brief Souls living here.
     *
     * Derived from the building count rather than invented: each dwelling holds
     * a drawn number of occupants, scaled by tier density and by how well the
     * land feeds them. A settlement's population and its footprint therefore
     * cannot disagree.
     */
    int population = 0;
    /** @brief Relative wealth in `[0, 1]`, from site quality and access to trade. */
    double prosperity = 0.0;

    std::vector<MapBuilding> buildings; /**< @brief Footprints that fit inside the cell polygon. */
    /**
     * @brief The lanes its buildings front onto, one fan per claimed cell.
     *
     * Derived from the roads and rivers touching each claimed cell, so a
     * settlement's streets meet the network that reaches it rather than being
     * invented beside it.
     */
    std::vector<MapStreet> streets;
    /**
     * @brief Its market square, or a zero radius when it has none.
     *
     * Granted by tier and by whether the ground is actually there -- see
     * `TownConfig::plaza_radius_m`. The streets already converge on the primary
     * cell's site, so siting the square there makes them radiate from it.
     */
    MapPlaza plaza;
};

/**
 * @brief The four corners of a building's footprint, with its yaw applied.
 *
 * Returned counter-clockwise from the footprint's local bottom-left. This is
 * the authoritative shape of a building: `MapBuilding::point` is only its
 * centre, and the generator guarantees containment and non-overlap against
 * *these* corners, not against an axis-aligned box.
 *
 * @param building The building to expand.
 * @return Its four corners in world grid units.
 */
inline std::array<MapPoint, 4> building_corners(const MapBuilding& building) {
    const double half_w = building.width * 0.5;
    const double half_h = building.height * 0.5;
    const double c = std::cos(building.rotation);
    const double s = std::sin(building.rotation);

    const double local[4][2] = {{-half_w, -half_h}, {half_w, -half_h},
                                {half_w, half_h},   {-half_w, half_h}};
    std::array<MapPoint, 4> corners{};
    for (std::size_t i = 0; i < 4; ++i) {
        corners[i] = {building.point.x + local[i][0] * c - local[i][1] * s,
                      building.point.y + local[i][0] * s + local[i][1] * c};
    }
    return corners;
}

/**
 * @brief Separating-axis test for two oriented footprints.
 *
 * Two convex shapes miss each other exactly when some axis separates their
 * projections, and for rectangles only the four edge normals can be that axis.
 * A bounding-circle test would be cheaper but rejects far more than it needs
 * to, which would stop buildings from lining a street closely enough to read
 * as one.
 *
 * @param a First footprint.
 * @param b Second footprint.
 * @return True if the two footprints intersect.
 */
inline bool buildings_overlap(const MapBuilding& a, const MapBuilding& b) {
    const std::array<MapPoint, 4> box_a = building_corners(a);
    const std::array<MapPoint, 4> box_b = building_corners(b);

    for (int shape = 0; shape < 2; ++shape) {
        const std::array<MapPoint, 4>& source = shape == 0 ? box_a : box_b;
        for (std::size_t i = 0; i < 2; ++i) {
            // Edge normal; only two per rectangle are distinct.
            const double axis_x = -(source[i + 1].y - source[i].y);
            const double axis_y = source[i + 1].x - source[i].x;
            const double length = std::hypot(axis_x, axis_y);
            if (length == 0.0) {
                continue;
            }
            const double nx = axis_x / length;
            const double ny = axis_y / length;

            double min_a = 0.0, max_a = 0.0, min_b = 0.0, max_b = 0.0;
            for (std::size_t k = 0; k < 4; ++k) {
                const double pa = box_a[k].x * nx + box_a[k].y * ny;
                const double pb = box_b[k].x * nx + box_b[k].y * ny;
                if (k == 0) {
                    min_a = max_a = pa;
                    min_b = max_b = pb;
                } else {
                    min_a = std::min(min_a, pa); max_a = std::max(max_a, pa);
                    min_b = std::min(min_b, pb); max_b = std::max(max_b, pb);
                }
            }
            if (max_a <= min_b || max_b <= min_a) {
                return false; // This axis separates them.
            }
        }
    }
    return true;
}

/**
 * @struct MapLandmark
 * @brief A notable place worth putting on a map and worth travelling to.
 */
struct MapLandmark {
    CenterId center = k_invalid_id;  /**< @brief The cell it stands on. */
    MapPoint point;                  /**< @brief Its position, in grid units. */
    LandmarkKind kind = LandmarkKind::Ruins; /**< @brief What sort of place it is. */
    std::string name;                /**< @brief Named in the dialect of its region. */
    RegionId region = k_invalid_id;  /**< @brief The region it lies in, or invalid. */
};

/**
 * @brief The language seed for a country, from the map seed and its id.
 *
 * Shared so the region pass and the town pass derive the same language without
 * passing one between them.
 *
 * @param map_seed `MapConfig::seed`.
 * @param country The country's id.
 * @return A stable seed for `language_for()`.
 */
inline std::uint32_t country_language_seed(int map_seed, CountryId country) {
    return static_cast<std::uint32_t>(map_seed) * 2654435761u
         + static_cast<std::uint32_t>(country + 1) * 40503u;
}

/**
 * @brief The dialect seed for a region, from the map seed and its id.
 * @param map_seed `MapConfig::seed`.
 * @param region The region's id.
 * @return A stable seed for `dialect_for()`.
 */
inline std::uint32_t region_dialect_seed(int map_seed, RegionId region) {
    return static_cast<std::uint32_t>(map_seed) * 2246822519u
         + static_cast<std::uint32_t>(region + 1) * 3266489917u;
}

/**
 * @struct MapRegion
 * @brief A province: a contiguous block of cells inside one country.
 *
 * Regions are the unit a settlement belongs to and takes its name from. Each
 * carries a dialect of its country's language, so its towns sound related to
 * their neighbours without being identical.
 */
struct MapRegion {
    RegionId index = k_invalid_id;   /**< @brief This region's own index in `MapGraph::regions`. */
    CountryId country = k_invalid_id;/**< @brief The country this region belongs to. */
    std::string name;                /**< @brief Generated in the region's own dialect. */

    CenterId seed = k_invalid_id;    /**< @brief The cell the region grew outward from. */
    CenterId capital = k_invalid_id; /**< @brief Its foremost settlement's cell, or invalid. */
    std::vector<CenterId> cells;     /**< @brief Every cell claimed by this region. */

    Biome dominant_biome = Biome::Grassland; /**< @brief The biome covering the most of it. */
    int population = 0;              /**< @brief Sum of its settlements' populations. */
    double area = 0.0;               /**< @brief Total cell area, in square grid units. */
    glm::vec3 color = glm::vec3(255.0f); /**< @brief Tint used when regions are drawn. */
};

/**
 * @struct MapCountry
 * @brief A nation: one or more regions under a single language.
 */
struct MapCountry {
    CountryId index = k_invalid_id;  /**< @brief This country's own index in `MapGraph::countries`. */
    std::string name;                /**< @brief Generated in the country's language. */

    std::vector<RegionId> regions;   /**< @brief The regions it is divided into. */
    CenterId capital = k_invalid_id; /**< @brief Its largest settlement's cell, or invalid. */
    int population = 0;              /**< @brief Sum of its regions' populations. */
    double area = 0.0;               /**< @brief Total claimed area, in square grid units. */
    glm::vec3 color = glm::vec3(255.0f); /**< @brief Tint used when countries are drawn. */
};

/**
 * @struct CaveNode
 * @brief One station along a cave passage: where it is, and the rock it sits between.
 *
 * The first thing in this module that has a position *underground* rather than
 * on the surface, and it says so by carrying two heights instead of one.
 * `floor` and `roof` are in the same normalised `[0, 1]` field every other
 * elevation in the graph uses, so they are directly comparable with
 * `MapCenter::elevation`, with `water_level`, and with whatever
 * `MapGraph::elevation_at()` returns overhead -- which is the comparison the
 * whole feature rests on.
 *
 * `center` is the cell the node lies in, kept because `elevation_at()` wants a
 * hint and re-deriving one per sample is the expensive way to ask.
 */
struct CaveNode {
    MapPoint point;                 /**< @brief Plan position, in grid units. */
    double floor = 0.0;             /**< @brief Height of the floor, normalised like all elevation. */
    double roof = 0.0;              /**< @brief Height of the ceiling; always above `floor`. */
    double radius = 0.0;            /**< @brief Half-width of the passage here, in grid units. */
    CenterId center = k_invalid_id; /**< @brief The cell it sits in; the hint `elevation_at()` takes. */
    CaveZone zone = CaveZone::Vadose;            /**< @brief Which regime cut it. */
    CaveFeature feature = CaveFeature::Passage;  /**< @brief What kind of space it is. */
    /**
     * @brief Which of `MapCave::levels` the station belongs to; 0 is the shallowest.
     *
     * Recorded rather than derived from `floor`, for the reason `cave.h` gives for
     * recording `CaveZone`: inference would have to re-derive the system's whole
     * table sequence, which is a property of where the mouth opened and not of the
     * station in hand. A shaft between two levels takes the level it descends
     * *into*, so every level is its network plus the way down to it and no station
     * belongs to two.
     */
    std::int32_t level = 0;
    /** @brief Index of the node this one was grown from within `MapCave::nodes`; -1 at the mouth. */
    std::int32_t parent = -1;
};

/**
 * @struct CavePassage
 * @brief One unbranched run of a cave, from a junction to the next junction or an end.
 *
 * Two representations of the same run, for the same reason `MapRiver` keeps two:
 * `nodes` is the chain as it was grown and is what the geometry is *true* of,
 * while the four parallel arrays are its corner-cut copy and are what gets drawn.
 * Drawn straight between stations a passage is visibly faceted at every one, which
 * is a shape no watercourse ever cut.
 *
 * `points`, `floors`, `roofs` and `radii` are always the same length: smoothing
 * moves a station's position, its two heights and its width together, so a
 * consumer can read any index across all four and get one coherent cross-section.
 */
struct CavePassage {
    /**
     * @brief Which of `MapCave::levels` this run was cut at; 0 is the shallowest.
     *
     * A run belongs to exactly one storey, because it is one growing head's work
     * and a head never changes the table it is cutting to. Recorded here as well
     * as on every station so a consumer can select a whole storey -- "give me the
     * upper level of this system" -- without walking into `nodes` to ask.
     *
     * The chain's first entry is the station the run was grown from, which for a
     * descent is on the level above; every other entry is on this one.
     */
    std::int32_t level = 0;
    std::vector<std::int32_t> nodes;  /**< @brief The grown chain, indices into `MapCave::nodes`. */
    std::vector<MapPoint> points;     /**< @brief Smoothed plan centreline. */
    std::vector<double> floors;       /**< @brief Smoothed floor per point, re-clamped under the terrain. */
    std::vector<double> roofs;        /**< @brief Smoothed ceiling per point, re-clamped likewise. */
    std::vector<double> radii;        /**< @brief Smoothed half-width per point, in grid units. */
};

/**
 * @struct MapCave
 * @brief One cave system, from the slope it opens on to the deepest passage it reaches.
 *
 * A system is a tree rooted at its mouth, not a single line: `nodes` owns every
 * station and each names its parent, while `passages` slices that tree into the
 * unbranched runs a renderer or a mesher actually wants to walk.
 *
 * `levels` is recorded rather than recomputed because it is a property of the
 * *system* -- fixed by where the mouth opened and how deep the configuration lets
 * a cave reach -- and nothing about a passage in hand can recover it. It is also
 * what explains the shape: around each table, water already at rest cut level,
 * branching network, and between one table and the next, water falling under
 * gravity cut the steep way down.
 */
struct MapCave {
    EdgeId mouth_edge = k_invalid_id; /**< @brief The steep edge it opened on. */
    MapPoint mouth;                   /**< @brief That edge's midpoint -- literally between two cells. */
    double mouth_grade = 0.0;         /**< @brief Rise over run there; why this edge was chosen. */
    double surface_at_mouth = 0.0;    /**< @brief Ground height at the mouth, for reference. */
    double phreatic_level = 0.0;      /**< @brief Height at which the descent flattens out; `levels.front()`. */
    /**
     * @brief Every water table the system was cut at, shallowest first.
     *
     * A cave has more than one level because the valley it drains to cut down and
     * took the water table with it: the phreatic network at the higher level was left
     * behind as dry passage, and a new one formed below. Each entry here is one
     * such stage, so the size of this is how many storeys the system has.
     *
     * Derived from the relief beneath the mouth rather than configured, which is
     * why a cave high in the hills has three levels and one near the coast has
     * one -- there has to be something for the table to fall through.
     */
    std::vector<double> levels;
    double deepest = 0.0;             /**< @brief Lowest floor anywhere in the system. */
    double length_m = 0.0;            /**< @brief Total passage length, in metres. */
    RegionId region = k_invalid_id;   /**< @brief The region it opens in; supplies its dialect. */
    std::string name;                 /**< @brief Generated in that dialect. */

    std::vector<CaveNode> nodes;      /**< @brief Every station, parent-linked into a tree. */
    std::vector<CavePassage> passages;/**< @brief That tree sliced into unbranched runs. */
};

/**
 * @struct TerrainDetail
 * @brief A fractal field sampled on top of the control mesh, and how hard to apply it.
 *
 * Built from a `MapConfig` by `make_terrain_detail()` and handed to
 * `MapGraph::elevation_at()`. Holds a borrowed pointer rather than a `Noise` by
 * value so one sampler serves a whole render: `Noise` wraps a FastNoiseLite,
 * which is not free to construct.
 */
struct NaturalTerrain;

struct TerrainDetail {
    const Noise* noise = nullptr; /**< @brief The detail field; null disables displacement. */
    double roughness = 0.0;       /**< @brief Displacement amplitude; 0 disables it. */
    /** @brief `TerrainModel::Natural`'s landforms; null samples the classic surface. See `TerrainSurface`. */
    const NaturalTerrain* natural = nullptr;
};

/**
 * @struct NaturalTerrain
 * @brief What `TerrainModel::Natural` shapes the sampled ground with: its landform fields, and
 *        how far each cell is from water (which decides where the shaping may lower the ground).
 *
 * Built by `TerrainSurface` from a generated graph; read-only afterwards, so any number of
 * threads may sample through it.
 */
struct NaturalTerrain {
    NaturalTerrainConfig config;
    double sea_level = 0.25;
    Noise hills;
    Noise ridge;
    Noise plateau;
    Noise warp;
    Noise seabed;
    /** @brief Per cell: rings of cells out from the nearest sea or lake cell, 0 on the water, capped at 4. */
    std::vector<std::uint8_t> rings;
    /**
     * @brief How much local depth, normalised, counts as safe from sample-time noise erosion.
     *
     * Reuses `lake_depth_m` as a general scale for any water body a point falls under -- a lake,
     * a river pocket, a shallow sea shelf -- not a second carving pass; the basin shape itself is
     * still cut only for lakes, in `assign_water_levels()`. `natural_elevation_()` reads this to
     * keep the land and bed shaping below from raising a point back out of water deeper than it.
     */
    double min_protected_depth = 0.0;

    NaturalTerrain(const MapConfig& map, std::vector<std::uint8_t> water_rings)
        : config(map.natural), sea_level(map.sea_level), hills(map.noise_hills), ridge(map.noise_ridge),
          plateau(map.noise_plateau), warp(map.noise_warp), seabed(map.noise_seabed),
          rings(std::move(water_rings)),
          min_protected_depth(meters_to_height(map, std::max(0.0, map.lake_depth_m))) {}
};

/**
 * @brief Builds the detail sampler `MapGraph::elevation_at()` displaces with.
 *
 * The `Noise` must outlive every sample taken through the returned struct, which
 * only borrows it -- build both once per render, not per pixel.
 *
 * @param config Supplies the roughness amplitude.
 * @param noise A sampler built from `MapConfig::noise_terrain`.
 * @return The detail field, or a disabled one when roughness is zero.
 */
inline TerrainDetail make_terrain_detail(const MapConfig& config, const Noise& noise) {
    TerrainDetail detail;
    if (config.terrain_roughness > 0.0) {
        detail.noise = &noise;
        detail.roughness = config.terrain_roughness;
    }
    return detail;
}

/**
 * @struct ChannelSegment
 * @brief One straight piece of a river centreline, and the cut it carries.
 *
 * Taken from `MapRiver::points` -- the *smoothed* centreline, the same polyline
 * the water layer strokes -- so the channel in the height field and the ribbon
 * drawn over it register instead of drifting apart at every bend.
 */
struct ChannelSegment {
    MapPoint a;              /**< @brief One end, in grid units. */
    MapPoint b;              /**< @brief The other end, in grid units. */
    double half_width = 0.0; /**< @brief Half the river's width here, in grid units. */
    double depth = 0.0;      /**< @brief Depth of the cut at the centreline, in height units. */
    /**
     * @brief Fraction of `half_width` held at full depth before the bed rises, 0 to 1.
     *
     * Zero upstream, where the bed is a plain concave channel. It opens toward 1 at
     * an estuary, because a wider parabola does not help there: at 94% of the radius
     * a parabolic bed has risen back to within 11% of the rim, so the edges of the
     * drawn ribbon would still rest on the bank. A flat bed is what actually puts
     * the whole mouth at the waterline.
     */
    double flat = 0.0;
    std::int32_t river = -1;   /**< @brief Index into `MapGraph::rivers`. */
    /**
     * @brief Which segment of that river, as `RiverSurfaces` indexes it. One past the course's
     *        last -- `points.size() - 1` -- is the mouth: the channel carried on into the body
     *        the river feeds, which only `RiverChannels::banked` has a height for.
     */
    std::int32_t segment = -1;
    bool mouth = false; /**< @brief This is that mouth segment: its water is level with the body's. */
};

/**
 * @brief How far past the drawn ribbon an estuary is widened, as a multiple of its reach.
 *
 * The probe that decides the water's floor samples the stroke's corners, which lie
 * about 1.4 reaches from the centreline, so a channel widened to exactly one reach
 * still leaves them on the bank. One and a half covers them with a margin.
 */
inline constexpr double k_estuary_reach = 2.0;

/**
 * @brief How much of an estuary's width is flat bed at the mouth, 0 to 1.
 *
 * The probe that decides the water's floor samples the stroke's corners, which lie
 * about 1.4 reaches out. At this fraction of a `k_estuary_reach`-wide channel the
 * flat bed extends to 1.6 reaches, so those corners are over the bed rather than the
 * bank, and the sheet can sit at the waterline across its whole width.
 */
inline constexpr double k_estuary_flat = 0.8;

/**
 * @struct RiverChannels
 * @brief The river channels cut into the sampled surface, indexed by cell.
 *
 * The counterpart of `TerrainDetail`: both are surface features too fine for the
 * control mesh to hold, applied by `MapGraph::elevation_at()` at sample time and
 * never written back to cell or corner heights. Biomes, rivers and roads go on
 * classifying on the mesh, while anyone sampling the *surface* gets the channel.
 *
 * Built once per render by `make_river_channels()`, never per pixel. Segments are
 * stored flat and grouped by cell, and each cell carries a bounding box of its own
 * group so the sampler can reject the overwhelming majority of pixels -- rivers
 * cover very little of a map -- with one comparison instead of a distance loop.
 */
struct RiverChannels {
    /** @brief One cell's slice of `segments`, and the box that bounds it. */
    struct Cell {
        std::size_t first = 0; /**< @brief Index of this cell's first segment. */
        std::size_t count = 0; /**< @brief How many segments follow it. */
        MapPoint min;          /**< @brief Low corner of the box, widened by `half_width`. */
        MapPoint max;          /**< @brief High corner of the box, widened by `half_width`. */
    };

    std::vector<ChannelSegment> segments; /**< @brief Every segment, grouped by cell. */
    std::vector<Cell> cells;              /**< @brief Indexed by `CenterId`. */
    /**
     * @brief Each river's physical water surface, one height per segment (`RiverFit::Banked`).
     *
     * Decided *before* the channel is cut, from the uncut ground: under both banks, falling
     * toward the mouth, and backed up to the body it feeds. The cut is then made deep enough to
     * hold water at that height, so the two always agree -- a channel can never run dry under
     * the water beside it, and the water can never stand over its banks.
     */
    std::vector<std::vector<double>> banked;

    /** @brief Whether any channel is cut at all. */
    bool empty() const { return segments.empty(); }
};

/**
 * @brief Distance from a point to a line segment, in grid units.
 *
 * The inner loop of the channel cut, so it is written without a square root
 * where one is avoidable and takes the endpoints by reference.
 *
 * @param x Horizontal grid position.
 * @param y Vertical grid position.
 * @param a One end of the segment.
 * @param b The other end.
 * @return The shortest distance from the point to the segment.
 */
inline double distance_to_segment(double x, double y, const MapPoint& a, const MapPoint& b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length_squared = dx * dx + dy * dy;
    double t = 0.0;
    if (length_squared > 0.0) {
        t = std::clamp(((x - a.x) * dx + (y - a.y) * dy) / length_squared, 0.0, 1.0);
    }
    const double px = x - (a.x + t * dx);
    const double py = y - (a.y + t * dy);
    return std::sqrt(px * px + py * py);
}

/**
 * @brief Distance from a point to a segment in 3D -- what a sphere map's channels and rivers are
 *        measured with (the chord, at river widths the arc to well under a percent).
 */
inline double distance_to_segment(const MapPoint& p, const MapPoint& a, const MapPoint& b) {
    const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const double length_squared = dx * dx + dy * dy + dz * dz;
    double t = 0.0;
    if (length_squared > 0.0) {
        t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy + (p.z - a.z) * dz) / length_squared, 0.0, 1.0);
    }
    const double px = p.x - (a.x + t * dx), py = p.y - (a.y + t * dy), pz = p.z - (a.z + t * dz);
    return std::sqrt(px * px + py * py + pz * pz);
}

/**
 * @class MapGraph
 * @brief Owns every cell, corner, edge, road run and settlement of one generated map.
 *
 * The three arrays are index-addressed and self-consistent: `centers[i].index
 * == i` holds for all three after generation, and every id stored in an
 * adjacency list is a valid slot in the corresponding array. Passes that
 * reorder anything must sort an index array instead of the storage itself.
 */
class MapGraph {
public:
    std::vector<MapCenter> centers; /**< @brief Voronoi cells, one per generating site. */
    std::vector<MapCorner> corners; /**< @brief Voronoi vertices, one per Delaunay triangle. */
    std::vector<MapEdge> edges;     /**< @brief Shared Delaunay/Voronoi edges. */
    std::vector<MapRoad> roads;     /**< @brief Road runs traced by the road pass. */
    std::vector<MapRiver> rivers;   /**< @brief Watercourses traced by the river pass. */
    std::vector<MapTown> towns;     /**< @brief Settlements placed by the town pass. */
    std::vector<MapRegion> regions; /**< @brief Provinces carved by the region pass. */
    std::vector<MapCountry> countries; /**< @brief Nations carved by the region pass. */
    std::vector<MapLandmark> landmarks; /**< @brief Notable places found by the landmark pass. */
    std::vector<MapCave> caves;     /**< @brief Cave systems opened by the cave pass. */
    /**
     * @brief A sphere map's radius in grid units (`MapTopology::Sphere`): its points lie on a
     *        globe of this radius about the origin. 0 for a plane map.
     */
    double sphere_radius = 0.0;

    /** @brief True for a sphere map. */
    bool is_sphere() const { return sphere_radius > 0.0; }

    /** @brief A noise field at a point: in 3D on a sphere (seamless round the globe), 2D on a plane. */
    float noise_at(const Noise& noise, const MapPoint& p) const {
        return is_sphere() ? noise.sample(p.x, p.y, p.z) : noise.sample(p.x, p.y);
    }

    /** @brief A point pushed back onto a sphere map's globe; unchanged on a plane. */
    MapPoint on_surface(const MapPoint& p) const {
        if (!is_sphere()) return p;
        const double len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len <= 0.0) return p;
        const double k = sphere_radius / len;
        return MapPoint{p.x * k, p.y * k, p.z * k};
    }

    /**
     * @brief The point `distance` grid units from `p` along `bearing` (radians, counter-clockwise
     *        from +x). On a plane that is exactly `p + (cos, sin) * distance`; on a globe the
     *        bearing is measured in the ground's local frame (from east toward north, with east
     *        along the equator), and the step lands back on the globe.
     */
    MapPoint step_from(const MapPoint& p, double bearing, double distance) const {
        if (!is_sphere()) {
            return MapPoint{p.x + std::cos(bearing) * distance, p.y + std::sin(bearing) * distance};
        }
        double ex = 0.0, ey = 0.0, ez = 0.0, nx = 0.0, ny = 0.0, nz = 0.0;
        local_frame_(p, ex, ey, ez, nx, ny, nz);
        const double c = std::cos(bearing) * distance;
        const double s = std::sin(bearing) * distance;
        return on_surface(MapPoint{p.x + ex * c + nx * s, p.y + ey * c + ny * s, p.z + ez * c + nz * s});
    }

    /** @brief The bearing from `a` toward `b`, as `step_from()` measures one. */
    double bearing_between(const MapPoint& a, const MapPoint& b) const {
        if (!is_sphere()) {
            return std::atan2(b.y - a.y, b.x - a.x);
        }
        double ex = 0.0, ey = 0.0, ez = 0.0, nx = 0.0, ny = 0.0, nz = 0.0;
        local_frame_(a, ex, ey, ez, nx, ny, nz);
        const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        return std::atan2(dx * nx + dy * ny + dz * nz, dx * ex + dy * ey + dz * ez);
    }

    /** @brief A globe's local ground frame at `p`: east (along the equator) and north. */
    void local_frame_(const MapPoint& p, double& ex, double& ey, double& ez, double& nx, double& ny,
                      double& nz) const {
        const double len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        const double ux = len > 0.0 ? p.x / len : 0.0, uy = len > 0.0 ? p.y / len : 0.0,
                     uz = len > 0.0 ? p.z / len : 1.0;
        ex = -uy; ey = ux; ez = 0.0;   // cross(z-axis, up)
        double el = std::sqrt(ex * ex + ey * ey);
        if (el < 1e-9) { ex = 1.0; ey = 0.0; el = 1.0; }   // at a pole any east will do
        ex /= el; ey /= el;
        nx = uy * ez - uz * ey; ny = uz * ex - ux * ez; nz = ux * ey - uy * ex;   // cross(up, east)
    }

    /** @brief Drops every array, returning the graph to its freshly constructed state. */
    void clear() {
        sphere_radius = 0.0;
        centers.clear();
        corners.clear();
        edges.clear();
        roads.clear();
        rivers.clear();
        towns.clear();
        regions.clear();
        countries.clear();
        landmarks.clear();
        caves.clear();
    }

    /**
     * @brief Interpolates the ground height at a point.
     *
     * Barycentric over the **Delaunay triangle** containing the point, blending
     * the three cell-site heights at its vertices. That is the natural
     * piecewise-linear surface through samples taken at the sites, and it is what
     * a terrain mesh built from this data would be.
     *
     * `center` is a hint, not a constraint: the triangles searched are the ones
     * incident to that cell, found through `MapCorner::touches` -- each corner of
     * a cell is the circumcentre of a Delaunay triangle that cell is a vertex of.
     * The nearest site to any point is always a vertex of the Delaunay triangle
     * containing it, so for a point in the cell the answer is in that set.
     *
     * ### Two alternatives, and why both are wrong
     *
     * **Inverse-distance weighting over the cell's corners** reads as a plateau.
     * Every corner is roughly equidistant from the middle of a cell, so the
     * interior comes out near the mean of the corners and only approaches a
     * corner's own value in the last few pixels before it.
     *
     * **Barycentric over the cell's own corner fan** -- site to two consecutive
     * corners -- is worse. It puts a crease at each of the six-odd internal fan
     * edges *and* a tent pole at every site, whose height is the mean of its
     * corners, so the surface comes out visibly crumpled. And the fan covers only
     * the *straight* corner polygon while the renderer draws the *subdivided*
     * outline, which bulges outside it: some 1.8% of drawn pixels miss every fan
     * triangle and need a fallback formula, speckling every cell boundary with
     * slivers of a different surface.
     *
     * The Delaunay triangulation has neither problem. It tiles the hull, so there
     * is no outside to fall through, and it creases once per edge rather than six
     * times per cell.
     *
     * @param center The cell to search from; the point need not be strictly inside it.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @return The interpolated height, or 0 if the cell has no corners.
     */
    double elevation_at(const MapCenter& center, double x, double y) const {
        return elevation_at(center, MapPoint{x, y});
    }

    /** @brief `elevation_at()` at a point given whole -- the form a sphere map is queried in. */
    double elevation_at(const MapCenter& center, const MapPoint& query) const {
        if (center.corners.empty()) {
            return 0.0;
        }

        double height = 0.0;
        if (triangle_elevation_(center, query, height)) {
            return height;
        }
        // The cell's own triangles cover its Voronoi region, but the renderer
        // draws the *subdivided* outline, which bulges past it into a
        // neighbour's -- about 2% of drawn pixels. Those belong to a
        // neighbour's triangles, so look there before giving up.
        for (const CenterId neighbor_id : center.neighbors) {
            if (triangle_elevation_(centers[static_cast<std::size_t>(neighbor_id)], query,
                                    height)) {
                return height;
            }
        }
        return inverse_distance_elevation_(center, query);
    }

    /**
     * @brief Interpolates the ground height, with fractal detail on top.
     *
     * The control mesh gives the landform; the detail field gives it texture.
     * Keeping the two apart is what stops the noise reaching the data everything
     * else is derived from -- biomes, rivers and roads all classify on the cell
     * and corner heights, which stay exactly as the passes left them, while
     * anyone sampling the *surface* sees the displaced version.
     *
     * The displacement is scaled by the base height, so it fades to nothing at
     * the coast: a shoreline stays exactly at sea level and no land is nudged
     * below it.
     *
     * @param center The cell to sample within.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @param detail The detail field and how strongly to apply it.
     * @return The displaced height, clamped to `[0, 1]`.
     */
    double elevation_at(const MapCenter& center, double x, double y,
                        const TerrainDetail& detail) const {
        return elevation_at(center, MapPoint{x, y}, detail);
    }

    /** @brief The detailed surface at a point given whole (a sphere map's form). */
    double elevation_at(const MapCenter& center, const MapPoint& p, const TerrainDetail& detail) const {
        if (detail.natural != nullptr) {
            return natural_elevation_(center, p, *detail.natural);
        }
        const double base = elevation_at(center, p);
        if (detail.noise == nullptr || detail.roughness <= 0.0) {
            return base;
        }
        // Taper by the base height itself. Anything that does not vanish at zero
        // would either lift the shoreline off sea level or push it under.
        const double amplitude = detail.roughness * base;
        const double displaced = base + amplitude * static_cast<double>(noise_at(*detail.noise, p));
        return std::clamp(displaced, 0.0, 1.0);
    }

    /**
     * @brief Interpolates the ground height, with detail and river channels on top.
     *
     * The full surface: the control mesh gives the landform, the detail field
     * gives it texture, and the channel cuts the watercourse into it. All three
     * are separate on purpose -- only the first is written back to cell and corner
     * heights, so biomes, rivers and roads keep classifying on a mesh that no
     * surface feature has touched.
     *
     * The channel is subtracted **last**, after the detail displacement, so
     * roughness cannot fill the bed back in. Without that ordering a rough map
     * would show a channel full of rubble rather than a channel.
     *
     * @param center The cell to sample within.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @param detail The detail field and how strongly to apply it.
     * @param channels The channels to cut, from `make_river_channels()`.
     * @return The displaced and cut height, clamped to `[0, 1]`.
     */
    double elevation_at(const MapCenter& center, double x, double y, const TerrainDetail& detail,
                        const RiverChannels& channels) const {
        return elevation_at(center, MapPoint{x, y}, detail, channels);
    }

    /** @brief The full surface at a point given whole (a sphere map's form). */
    double elevation_at(const MapCenter& center, const MapPoint& p, const TerrainDetail& detail,
                        const RiverChannels& channels) const {
        const double surface = elevation_at(center, p, detail);
        const double cut = channel_cut(center, p, channels);
        return cut > 0.0 ? std::clamp(surface - cut, 0.0, 1.0) : surface;
    }

    /**
     * @brief How deep the river channel runs beneath a point, in height units.
     *
     * Public, and named without the trailing underscore this codebase gives its
     * internal helpers, because the renderer reads it directly: the flat elevation
     * surface has no interpolation to carry a channel, so it subtracts one from the
     * cell's own height instead.
     *
     * Zero everywhere except within a river's own width of its centreline, which
     * is a very small part of a map -- hence the bounding-box test before the
     * distance loop. It runs once per pixel of the elevation layer, some 23
     * million times at the default render size, and the box is what keeps that
     * affordable.
     *
     * The profile is `depth * (1 - t*t)` against `t = distance / half_width`: a
     * concave bed that reaches exactly zero at the rim, so the cut meets the
     * surrounding ground without a step. The kink there is deliberate -- it is a
     * cut bank, and an edge is the only thing that reads as a river rather than
     * as a dip in the ground.
     *
     * Overlapping segments take the deepest rather than the sum, so a confluence
     * is one channel and a bend is not gouged twice. `max` of continuous
     * functions is continuous, so that costs nothing in smoothness.
     *
     * @param center The cell being sampled; selects the segment group.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @param channels The channel index.
     * @return The depth to subtract, or 0 away from any watercourse.
     */
    double channel_cut(const MapCenter& center, double x, double y,
                        const RiverChannels& channels) const {
        return channel_cut(center, MapPoint{x, y}, channels);
    }

    /** @brief `channel_cut()` at a point given whole (a sphere map's form). */
    double channel_cut(const MapCenter& center, const MapPoint& p, const RiverChannels& channels) const {
        const double x = p.x;
        const double y = p.y;
        const std::size_t index = static_cast<std::size_t>(center.index);
        if (index >= channels.cells.size()) {
            return 0.0;
        }
        const RiverChannels::Cell& cell = channels.cells[index];
        if (cell.count == 0 || x < cell.min.x || x > cell.max.x || y < cell.min.y
            || y > cell.max.y) {
            return 0.0;
        }
        const bool sphere = is_sphere();
        if (sphere && (p.z < cell.min.z || p.z > cell.max.z)) {
            return 0.0;
        }

        double deepest = 0.0;
        for (std::size_t i = cell.first; i < cell.first + cell.count; ++i) {
            const ChannelSegment& segment = channels.segments[i];
            const double distance = sphere ? distance_to_segment(p, segment.a, segment.b)
                                           : distance_to_segment(x, y, segment.a, segment.b);
            if (distance >= segment.half_width) {
                continue;
            }
            const double t = distance / segment.half_width;
            if (t <= segment.flat) {
                deepest = std::max(deepest, segment.depth);
                continue;
            }
            // Beyond the flat bed the same concave rise as before, re-based so it
            // still reaches exactly zero at the rim.
            const double span = 1.0 - segment.flat;
            const double u = span > 0.0 ? (t - segment.flat) / span : 1.0;
            deepest = std::max(deepest, segment.depth * (1.0 - u * u));
        }
        return deepest;
    }

    /**
     * @brief `TerrainModel::Natural`'s ground at a point.
     *
     * **The base** joins the three cell heights of the Delaunay triangle smoothly: each
     * barycentric weight is eased (`smoothstep`, by `smoothing`) before the blend, so the
     * surface still passes through every cell's own height and never leaves the range of its
     * triangle's three, but rounds over instead of running in planes.
     *
     * **The landforms** are added over it, sampled through a warped field so ridges and
     * valleys bend: rolling hills on the lowland, ridged mountain ranges where the land is high
     * (`mountain_start`, the boundary itself broken up), and flat-topped plateaus with steep
     * edges on a share of the lowland away from the mountains.
     *
     * **Near water the shaping may only raise the ground**: how far a point is from the sea or
     * a lake (in cells, blended over the triangle) fades out first the lowering, then the
     * raising. Every shore therefore stands at least as high as the classic surface puts it,
     * and that surface keeps every shore above its water -- the guarantee `WaterField` relies on.
     *
     * **Under the water the bed gets its own landforms**: rolling relief (`seabed`) faded in over
     * the first `seabed_shelf` of depth, and ridges (`seabed_ridges`) faded in past it, both still
     * zero in the open sea (never carved, so there is no depth there to protect) and near every
     * shoreline (where the surface must stay free to taper smoothly into the water). Past that
     * band, once a point's carved depth reaches `lake_depth_m`, a floor keeps the land and bed
     * shaping above from raising the point back out of water deeper than that -- see
     * `min_protected_depth` below.
     */
    double natural_elevation_(const MapCenter& center, const MapPoint& p, const NaturalTerrain& n) const {
        const double x = p.x;
        const double y = p.y;
        std::array<CenterId, 3> ids{};
        std::array<double, 3> weights{};
        if (!triangle_at(center, p, ids, weights)) {
            return elevation_at(center, p);
        }
        const auto smoothstep = [](double e0, double e1, double v) {
            const double t = std::clamp((v - e0) / (e1 - e0), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        };
        const double smoothing = std::clamp(n.config.smoothing, 0.0, 1.0);
        double base = 0.0;
        double total = 0.0;
        double ring = 0.0;
        // The highest water any of the triangle's cells puts over this point -- the same reach
        // `WaterField::at()` reads the triangle for, so the two agree on where the shoreline is.
        double water = 0.0;
        for (std::size_t i = 0; i < 3; ++i) {
            const double w = std::clamp(weights[i], 0.0, 1.0);
            const double eased = w + (w * w * (3.0 - 2.0 * w) - w) * smoothing;
            const std::size_t id = static_cast<std::size_t>(ids[i]);
            base += eased * centers[id].elevation;
            total += eased;
            ring += w * static_cast<double>(id < n.rings.size() ? n.rings[id] : 4);
            water = std::max(water, centers[id].water_level);
        }
        base = total > 0.0 ? base / total : elevation_at(center, p);
        // The depth the carving passes actually gave this point, before any shaping below touches
        // it -- the budget the final floor spends down as a point's shaped ground tries to exceed it.
        const double headroom0 = water - base;

        // Both shapings are gated to nothing in places, and the gate is cheaper than the fields
        // it guards -- four noise lookups for the land, two for the bed, against a render that
        // takes 23 million samples. So each is tested before it is sampled.
        //
        // The LAND shaping is exactly zero wherever the blended ring is: `raise` and `lower` are
        // both smoothstep(., ., 0), whatever the amplitudes say. A ring of zero means every cell
        // of the triangle is a water cell, which is the open sea and most of a map.
        const bool shape_land = ring > 0.0;
        // The BED shaping needs water over the point to have anywhere to apply; `water` is zero
        // on any triangle that neither touches a body nor dips under the waterline.
        const bool shape_bed = water > 0.0 && (n.config.seabed > 0.0 || n.config.seabed_ridges > 0.0);

        // One shaped value, however it got there, so the depth floor below applies no matter
        // which branch ran -- a later change to either shaping cannot reopen the leak on just one
        // of today's three return paths without also touching this one.
        double shaped = base;
        if (shape_land || shape_bed) {
            // The fields are read through a warp: 2D on a plane, 3D (so it wraps the globe) on a sphere.
            const bool sphere = is_sphere();
            const auto at = [sphere](const Noise& field, double a, double b, double c) {
                return static_cast<double>(sphere ? field.sample(a, b, c) : field.sample(a, b));
            };
            const double z = p.z;
            const double wx = x + n.config.warp * at(n.warp, x, y, z);
            const double wy = y + n.config.warp * at(n.warp, x + 37.1, y - 21.7, z + 13.3);
            const double wz = sphere ? z + n.config.warp * at(n.warp, x - 19.4, y + 8.8, z - 41.0) : 0.0;

            double ground = base;
            if (shape_land) {
                const double sea = n.sea_level;
                const double land = sea < 1.0 ? std::clamp((base - sea) / (1.0 - sea), 0.0, 1.0) : 0.0;
                const double hills = n.config.hills * at(n.hills, wx, wy, wz) * smoothstep(0.0, 0.12, land);
                const double edge = land + 0.12 * at(n.plateau, x * 0.5 + 411.0, y * 0.5 - 97.0, z * 0.5 + 53.0);
                const double range = smoothstep(n.config.mountain_start, n.config.mountain_start + 0.3, edge);
                // Squared, so the crests come to an edge and the slopes below them fall away concave.
                const double crest = 0.5 * (at(n.ridge, wx, wy, wz) + 1.0);
                const double mountains = n.config.mountains * range * (crest * crest - 0.25);
                const double threshold = 0.4 - 0.8 * std::clamp(n.config.plateau_coverage, 0.0, 1.0);
                const double mesa = smoothstep(threshold, threshold + 0.05, at(n.plateau, wx, wy, wz))
                                  * smoothstep(0.03, 0.12, land) * (1.0 - range);
                const double shaping = hills + mountains + n.config.plateaus * mesa;

                const double raise = smoothstep(0.0, 1.5, ring);
                const double lower = smoothstep(1.5, 3.0, ring);
                ground = base + (shaping > 0.0 ? raise : lower) * shaping;
            }

            // --- Under the water ---
            // Added to the land shaping rather than replacing it, and gated so it vanishes at the
            // waterline: the two are then one continuous surface, where a branch at the shoreline
            // would leave a step wherever the land shaping is non-zero there -- which it is around
            // every lake, since a lake stands above `sea_level` and so reads as land height.
            const double headroom = water - ground;
            if (shape_bed && headroom > 0.0) {
                const double shelf = std::max(1e-6, n.config.seabed_shelf);
                const double rolling = n.config.seabed * at(n.seabed, wx, wy, wz) * smoothstep(0.0, shelf, headroom);
                // The ridged field again, at an offset, so a bank reads like a range and the gullies
                // between them like the valleys -- but only out past the shelf, so a shore stays smooth.
                const double bank = 0.5 * (at(n.ridge, wx + 613.0, wy - 457.0, wz + 271.0) + 1.0);
                const double ridges = n.config.seabed_ridges * (bank * bank - 0.25)
                                    * smoothstep(shelf, 3.0 * shelf, headroom);
                double relief = rolling + ridges;
                // Room either way, so the bed stays a bed. Up: never more than halfway to the surface,
                // or a shoal would break it and strand dry ground in open water. Down: never more than
                // halfway to the floor of the height field, where a clamp would flatten it again --
                // which is why the deep floor rises into seamounts rather than being gouged, and an
                // abyssal plain is the flattest ground there is.
                relief = relief > 0.0 ? std::min(relief, 0.5 * headroom) : std::max(relief, -0.5 * ground);
                ground += relief;
            }
            shaped = ground;
        }

        // A depth floor, applied last and uniformly regardless of which shaping ran above. Once a
        // point's CARVED depth (`headroom0`, measured before any shaping) reaches the generator's
        // own configured minimum, the land and bed shaping together may no longer erase more than
        // a shrinking fraction of it. Right at the shoreline (`headroom0` -> 0) this is a no-op by
        // construction, so the surface still tapers smoothly into the water exactly as before; a
        // dry point never trips it either, since `assign_water_levels()` keeps every shore above
        // its own lake's spill surface, so `headroom0 <= 0` there. Only ground the carving already
        // called submerged is protected.
        if (headroom0 > 0.0 && n.min_protected_depth > 0.0) {
            const double reach = std::clamp(headroom0 / n.min_protected_depth, 0.0, 1.0);
            const double protect = 0.8 * reach;   // 0 at the shoreline, up to 80% once reach hits 1
            const double floor = water - headroom0 * protect;
            shaped = std::min(shaped, floor);
        }
        return std::clamp(shaped, 0.0, 1.0);
    }

    /**
     * @brief The Delaunay triangle containing a point: its three cells and their weights.
     *
     * The triangle `elevation_at()` interpolates over, exposed so a consumer can ask *which*
     * cells a sample was blended from -- the water query uses it to tell whether a point's
     * ground is part of a body's basin. Searches the hint cell's star, then its neighbours'.
     *
     * @param center The cell to search from.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @param out_cells Receives the triangle's three cells.
     * @param out_weights Receives their barycentric weights.
     * @return False when no triangle claims the point (only at the convex hull).
     */
    bool triangle_at(const MapCenter& center, double x, double y, std::array<CenterId, 3>& out_cells,
                     std::array<double, 3>& out_weights) const {
        return triangle_at(center, MapPoint{x, y}, out_cells, out_weights);
    }

    /** @brief `triangle_at()` at a point given whole (a sphere map's form). */
    bool triangle_at(const MapCenter& center, const MapPoint& query, std::array<CenterId, 3>& out_cells,
                     std::array<double, 3>& out_weights) const {
        if (triangle_cells_(center, query, out_cells, out_weights)) {
            return true;
        }
        for (const CenterId neighbor_id : center.neighbors) {
            if (triangle_cells_(centers[static_cast<std::size_t>(neighbor_id)], query, out_cells,
                                out_weights)) {
                return true;
            }
        }
        return false;
    }

    /** @brief `triangle_at()` over one cell's star only. */
    bool triangle_cells_(const MapCenter& center, const MapPoint& query,
                         std::array<CenterId, 3>& out_cells, std::array<double, 3>& out_weights) const {
        for (const CornerId corner_id : center.corners) {
            const MapCorner& corner = corners[static_cast<std::size_t>(corner_id)];
            if (corner.touches.size() != 3) {
                continue;
            }
            const MapCenter& a = centers[static_cast<std::size_t>(corner.touches[0])];
            const MapCenter& b = centers[static_cast<std::size_t>(corner.touches[1])];
            const MapCenter& c = centers[static_cast<std::size_t>(corner.touches[2])];
            if (barycentric_on_(a.point, b.point, c.point, query, out_weights[0], out_weights[1],
                             out_weights[2])) {
                out_cells = {a.index, b.index, c.index};
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Interpolates over whichever of a cell's Delaunay triangles claims a point.
     *
     * A cell's corners are exactly the circumcentres of the Delaunay triangles it
     * is a vertex of, so `MapCorner::touches` enumerates that cell's whole star.
     *
     * @param center The cell whose triangles to try.
     * @param query The point to sample.
     * @param out_height Receives the interpolated height when one claims it.
     * @return True if a triangle contained the point.
     */
    bool triangle_elevation_(const MapCenter& center, const MapPoint& query,
                             double& out_height) const {
        for (const CornerId corner_id : center.corners) {
            const MapCorner& corner = corners[static_cast<std::size_t>(corner_id)];
            if (corner.touches.size() != 3) {
                continue; // Not a complete triangle; only possible at the hull.
            }
            const MapCenter& a = centers[static_cast<std::size_t>(corner.touches[0])];
            const MapCenter& b = centers[static_cast<std::size_t>(corner.touches[1])];
            const MapCenter& c = centers[static_cast<std::size_t>(corner.touches[2])];

            double wa = 0.0;
            double wb = 0.0;
            double wc = 0.0;
            if (barycentric_on_(a.point, b.point, c.point, query, wa, wb, wc)) {
                out_height = wa * a.elevation + wb * b.elevation + wc * c.elevation;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Barycentric coordinates of a point in a triangle, if it falls inside.
     *
     * A small negative tolerance on each weight, so a sample sitting exactly on a
     * shared edge is claimed by a triangle rather than falling through every one
     * of them to the fallback.
     *
     * @param a First vertex.
     * @param b Second vertex.
     * @param c Third vertex.
     * @param point The point to locate.
     * @param out_a Weight of `a`.
     * @param out_b Weight of `b`.
     * @param out_c Weight of `c`.
     * @return True if the point lies in the triangle.
     */
    /** @brief `barycentric_()` on a plane; on a sphere, `barycentric3_()`. */
    bool barycentric_on_(const MapPoint& a, const MapPoint& b, const MapPoint& c, const MapPoint& point,
                         double& out_a, double& out_b, double& out_c) const {
        return is_sphere() ? barycentric3_(a, b, c, point, out_a, out_b, out_c)
                           : barycentric_(a, b, c, point, out_a, out_b, out_c);
    }

    /**
     * @brief Barycentric coordinates on a sphere map: the point's ray from the globe's centre
     *        meets the triangle's plane (a gnomonic projection, which keeps a spherical
     *        triangle's edges straight), and the weights are taken there.
     */
    static bool barycentric3_(const MapPoint& a, const MapPoint& b, const MapPoint& c, const MapPoint& point,
                              double& out_a, double& out_b, double& out_c) {
        const double e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
        const double e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
        const double nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
        const double nn = nx * nx + ny * ny + nz * nz;
        const double along = nx * point.x + ny * point.y + nz * point.z;
        if (nn == 0.0 || along == 0.0) return false;
        const double scale = (nx * a.x + ny * a.y + nz * a.z) / along;
        if (scale <= 0.0) return false;   // the triangle is on the far side of the globe
        const double qx = point.x * scale - a.x, qy = point.y * scale - a.y, qz = point.z * scale - a.z;
        // Weight of b: area(a, q, c) / area(a, b, c), signed against the normal; likewise c.
        const double cbx = qy * e2z - qz * e2y, cby = qz * e2x - qx * e2z, cbz = qx * e2y - qy * e2x;
        const double ccx = e1y * qz - e1z * qy, ccy = e1z * qx - e1x * qz, ccz = e1x * qy - e1y * qx;
        out_b = (cbx * nx + cby * ny + cbz * nz) / nn;
        out_c = (ccx * nx + ccy * ny + ccz * nz) / nn;
        out_a = 1.0 - out_b - out_c;
        constexpr double tolerance = -1e-9;
        return out_a >= tolerance && out_b >= tolerance && out_c >= tolerance;
    }

    static bool barycentric_(const MapPoint& a, const MapPoint& b, const MapPoint& c,
                             const MapPoint& point, double& out_a, double& out_b,
                             double& out_c) {
        const double v0x = b.x - a.x, v0y = b.y - a.y;
        const double v1x = c.x - a.x, v1y = c.y - a.y;
        const double denominator = v0x * v1y - v1x * v0y;
        if (denominator == 0.0) {
            return false; // Degenerate sliver; let the next triangle try.
        }
        const double px = point.x - a.x, py = point.y - a.y;
        out_b = (px * v1y - v1x * py) / denominator;
        out_c = (v0x * py - px * v0y) / denominator;
        out_a = 1.0 - out_b - out_c;

        constexpr double tolerance = -1e-9;
        return out_a >= tolerance && out_b >= tolerance && out_c >= tolerance;
    }

    /**
     * @brief The inverse-distance fallback, for a point no incident triangle claims.
     *
     * Only reachable at the convex hull, where a corner may not have three
     * touching cells and the triangulation runs out. Inside the map it should
     * never be hit -- `test_elevation_interpolation_has_no_gaps` is what holds
     * that to account.
     */
    double inverse_distance_elevation_(const MapCenter& center, const MapPoint& query) const {
        double total_weight = 0.0;
        double weighted_elevation = 0.0;
        for (const CornerId corner_id : center.corners) {
            const MapCorner& corner = corners[static_cast<std::size_t>(corner_id)];
            const double distance = query.distance_to(corner.point);
            if (distance == 0.0) {
                return corner.elevation;
            }
            const double weight = 1.0 / distance;
            weighted_elevation += weight * corner.elevation;
            total_weight += weight;
        }
        if (total_weight == 0.0) {
            return 0.0;
        }
        return weighted_elevation / total_weight;
    }

    /**
     * @brief Interpolates elevation along an edge from its two endpoint heights.
     * @param edge The edge to sample along.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @return The interpolated height, or 0 if either endpoint is unset.
     */
    double elevation_at(const MapEdge& edge, double x, double y) const {
        if (edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
            return 0.0;
        }
        const MapCorner& c0 = corners[static_cast<std::size_t>(edge.v0)];
        const MapCorner& c1 = corners[static_cast<std::size_t>(edge.v1)];

        const MapPoint query{x, y};
        const double distance_to_v0 = query.distance_to(c0.point);
        const double distance_to_v1 = query.distance_to(c1.point);
        const double total_distance = distance_to_v0 + distance_to_v1;

        if (total_distance == 0.0) {
            return c0.elevation;
        }
        return (distance_to_v1 / total_distance) * c0.elevation
             + (distance_to_v0 / total_distance) * c1.elevation;
    }

    /**
     * @brief Collects a cell's polygon outline in grid space, in winding order.
     *
     * Walks the cell's corners -- which the generator has already sorted
     * counter-clockwise -- and, for each consecutive pair, appends the whole
     * path of the edge joining them. Each edge stores its path as two halves
     * meeting at the midpoint, so one half is reversed to run the right way
     * round.
     *
     * Building the outline in order matters once edges are subdivided: a
     * wobbled path is not in convex position, so re-deriving the winding by
     * sorting the points about their centroid puts near-collinear points in the
     * wrong order and the fill tears. Cells whose edges do not form a closed
     * ring -- only possible at the outer hull -- fall back to the unordered
     * point set, which the caller sorts.
     *
     * @param center The cell to outline.
     * @return The polygon's vertices, in grid units and winding order.
     */
    std::vector<MapPoint> cell_outline(const MapCenter& center) const {
        std::vector<MapPoint> outline;
        const std::size_t corner_count = center.corners.size();

        if (corner_count >= 3) {
            bool complete = true;
            for (std::size_t i = 0; i < corner_count && complete; ++i) {
                const CornerId from = center.corners[i];
                const CornerId to = center.corners[(i + 1) % corner_count];

                const EdgeId edge_id = edge_between(center, from, to);
                if (edge_id == k_invalid_id) {
                    complete = false;
                    break;
                }

                const MapEdge& edge = edges[static_cast<std::size_t>(edge_id)];
                const bool forward = (edge.v0 == from);
                const std::vector<MapPoint>& first = forward ? edge.noisy_points0 : edge.noisy_points1;
                const std::vector<MapPoint>& second = forward ? edge.noisy_points1 : edge.noisy_points0;

                outline.insert(outline.end(), first.begin(), first.end());
                outline.insert(outline.end(), second.rbegin(), second.rend());
            }
            if (complete) {
                return outline;
            }
            outline.clear();
        }

        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = edges[static_cast<std::size_t>(edge_id)];
            outline.insert(outline.end(), edge.noisy_points0.begin(), edge.noisy_points0.end());
            outline.insert(outline.end(), edge.noisy_points1.begin(), edge.noisy_points1.end());
        }
        sort_points_radially(outline);
        return outline;
    }

    /**
     * @brief Polygon area of a cell, by the shoelace formula.
     *
     * Computed from `MapCenter::corners`, which the generator has already sorted
     * counter-clockwise, so the straight-edged cell is measured -- not the
     * wobbled one. That is the area a pass actually wants: subdivision moves a
     * boundary in and out about the same midpoint and leaves the enclosed area
     * essentially unchanged, and the straight polygon exists before the
     * noisy-edge pass has run.
     *
     * @param center The cell to measure.
     * @return Its area in square grid units; 0 for a degenerate cell.
     */
    double cell_area(const MapCenter& center) const {
        if (center.corners.size() < 3) {
            return 0.0;
        }
        double twice_area = 0.0;
        const std::size_t count = center.corners.size();
        if (is_sphere()) {
            // A fan of triangles from the site: the cell is all but flat at a grid unit across.
            const MapPoint& o = center.point;
            for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
                const MapPoint& a = corners[static_cast<std::size_t>(center.corners[i])].point;
                const MapPoint& b = corners[static_cast<std::size_t>(center.corners[j])].point;
                const double ax = a.x - o.x, ay = a.y - o.y, az = a.z - o.z;
                const double bx = b.x - o.x, by = b.y - o.y, bz = b.z - o.z;
                const double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
                twice_area += std::sqrt(cx * cx + cy * cy + cz * cz);
            }
            return twice_area * 0.5;
        }
        for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
            const MapPoint& a = corners[static_cast<std::size_t>(center.corners[i])].point;
            const MapPoint& b = corners[static_cast<std::size_t>(center.corners[j])].point;
            twice_area += (b.x + a.x) * (b.y - a.y);
        }
        return std::abs(twice_area) * 0.5;
    }

    /**
     * @brief Finds the edge of a cell joining two of its corners.
     *
     * Scans only the cell's own bounding edges, of which there are as many as
     * it has corners.
     *
     * @param center The cell whose boundary to search.
     * @param a One corner.
     * @param b The other corner.
     * @return The joining edge, or `k_invalid_id` if the corners are not adjacent.
     */
    EdgeId edge_between(const MapCenter& center, CornerId a, CornerId b) const {
        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = edges[static_cast<std::size_t>(edge_id)];
            if ((edge.v0 == a && edge.v1 == b) || (edge.v0 == b && edge.v1 == a)) {
                return edge_id;
            }
        }
        return k_invalid_id;
    }
};

/**
 * @struct SphereFrame
 * @brief A globe seen from straight above one point: the gnomonic projection onto the plane that
 *        touches the globe there, with x east and y north (`MapGraph::local_frame_`).
 *
 * What a pass that lays things out in 2D -- a town's plots and streets -- runs in on a sphere map:
 * flatten the graph around the place, lay out as on a plane, lift the result back. Within a few
 * cells of the touching point the projection stretches distances by well under a percent.
 */
struct SphereFrame {
    double radius = 0.0;
    double ux = 0.0, uy = 0.0, uz = 1.0;   ///< Up: the touching point's direction.
    double ex = 1.0, ey = 0.0, ez = 0.0;   ///< East.
    double nx = 0.0, ny = 1.0, nz = 0.0;   ///< North.

    SphereFrame(const MapGraph& graph, const MapPoint& at) : radius(graph.sphere_radius) {
        const double len = std::sqrt(at.x * at.x + at.y * at.y + at.z * at.z);
        if (len > 0.0) { ux = at.x / len; uy = at.y / len; uz = at.z / len; }
        graph.local_frame_(at, ex, ey, ez, nx, ny, nz);
    }

    /** @brief A point of the globe on the touching plane, in grid units from the touching point. */
    MapPoint to_plane(const MapPoint& p) const {
        const double along = p.x * ux + p.y * uy + p.z * uz;
        if (along <= radius * 0.05) return MapPoint{1.0e6, 1.0e6};   // the far side: out of reach
        const double k = radius / along;
        const double qx = p.x * k, qy = p.y * k, qz = p.z * k;
        return MapPoint{qx * ex + qy * ey + qz * ez, qx * nx + qy * ny + qz * nz};
    }

    /** @brief A point of the touching plane back on the globe. */
    MapPoint to_sphere(const MapPoint& q) const {
        const double px = ux * radius + ex * q.x + nx * q.y;
        const double py = uy * radius + ey * q.x + ny * q.y;
        const double pz = uz * radius + ez * q.x + nz * q.y;
        const double len = std::sqrt(px * px + py * py + pz * pz);
        const double k = len > 0.0 ? radius / len : 0.0;
        return MapPoint{px * k, py * k, pz * k};
    }

    /** @brief A copy of `graph` with every point on the touching plane -- a plane map to lay out on. */
    MapGraph flatten(const MapGraph& graph) const {
        MapGraph flat = graph;
        flat.sphere_radius = 0.0;
        for (MapCenter& c : flat.centers) c.point = to_plane(c.point);
        for (MapCorner& c : flat.corners) c.point = to_plane(c.point);
        for (MapEdge& e : flat.edges) {
            e.midpoint = to_plane(e.midpoint);
            for (MapPoint& p : e.noisy_points0) p = to_plane(p);
            for (MapPoint& p : e.noisy_points1) p = to_plane(p);
        }
        for (MapRiver& r : flat.rivers) for (MapPoint& p : r.points) p = to_plane(p);
        for (MapRoad& r : flat.roads) for (MapPoint& p : r.points) p = to_plane(p);
        return flat;
    }
};

/**
 * @class TerrainSurface
 * @brief Everything a consumer samples the ground through, built once per map: the detail
 *        field and, under `TerrainModel::Natural`, its landforms.
 *
 * `detail()` is what `MapGraph::elevation_at()` takes. Build one per render or bake -- the
 * renderer, the cave pass and a 3D host all do -- and pass the detail by reference; it borrows
 * from this object, which is therefore neither copyable nor movable.
 */
class TerrainSurface {
public:
    TerrainSurface(const MapGraph& graph, const MapConfig& config) : noise_(config.noise_terrain) {
        detail_ = make_terrain_detail(config, noise_);
        if (config.terrain_model == TerrainModel::Natural) {
            natural_ = std::make_unique<NaturalTerrain>(config, water_rings(graph));
            detail_.natural = natural_.get();
        }
    }
    TerrainSurface(const TerrainSurface&) = delete;
    TerrainSurface& operator=(const TerrainSurface&) = delete;

    const TerrainDetail& detail() const { return detail_; }

    /** @brief Per cell, rings of cells out from the nearest sea or lake cell (0 on water), capped at 4. */
    static std::vector<std::uint8_t> water_rings(const MapGraph& graph) {
        std::vector<std::uint8_t> rings(graph.centers.size(), 4);
        std::vector<CenterId> frontier;
        for (const MapCenter& c : graph.centers) {
            if (c.water) {
                rings[static_cast<std::size_t>(c.index)] = 0;
                frontier.push_back(c.index);
            }
        }
        for (std::uint8_t d = 1; d < 4 && !frontier.empty(); ++d) {
            std::vector<CenterId> next;
            for (const CenterId id : frontier) {
                for (const CenterId n : graph.centers[static_cast<std::size_t>(id)].neighbors) {
                    std::uint8_t& r = rings[static_cast<std::size_t>(n)];
                    if (r > d) {
                        r = d;
                        next.push_back(n);
                    }
                }
            }
            frontier = std::move(next);
        }
        return rings;
    }

private:
    Noise noise_;
    std::unique_ptr<NaturalTerrain> natural_;
    TerrainDetail detail_;
};

/**
 * @brief Steepness of the ground across an edge, as a dimensionless grade.
 *
 * The one definition of "how steep is it here".
 * The renderer's `hillshade_()` takes a gradient, but off the *blurred eight-bit
 * raster* and with a 600x exaggeration baked in, so it answers a question about
 * a picture rather than about the ground. `RoadConfig::slope_cost` multiplies a
 * bare height difference that is never divided by the distance it is spread
 * over, so it is only comparable between edges of similar length. And
 * `MapCorner::downslope` is a direction with no magnitude at all.
 *
 * Real metres both ways -- height through `height_to_meters()`, distance through
 * `grid_to_meters()` -- so 0.25 means a one-in-four slope and goes on meaning it
 * whatever `elevation_range_m` and `meters_per_grid_unit` are set to. A threshold
 * written against this is a statement about terrain rather than about units.
 *
 * Measured between the two cell *sites*, not the two corners: the sites are what
 * carry the smoothed heights the rest of the map is classified from, and an edge
 * is the boundary between them.
 *
 * @param graph The generated graph; reads `centers`.
 * @param edge The edge to measure across.
 * @param config Supplies the horizontal and vertical scales.
 * @return Rise over run, or 0 for a degenerate edge.
 */
inline double edge_grade(const MapGraph& graph, const MapEdge& edge, const MapConfig& config) {
    if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) {
        return 0.0;
    }
    const MapCenter& a = graph.centers[static_cast<std::size_t>(edge.d0)];
    const MapCenter& b = graph.centers[static_cast<std::size_t>(edge.d1)];
    const double run = grid_to_meters(config, a.point.distance_to(b.point));
    if (run <= 0.0) {
        return 0.0;
    }
    return height_to_meters(config, std::abs(a.elevation - b.elevation)) / run;
}

/**
 * @brief The level of the sea or lake that reaches a point, or 0 if none does.
 *
 * A body reaches every point whose Delaunay triangle -- the one `MapGraph::elevation_at()`
 * interpolates over -- has one of the body's cells as a vertex; where two could, the higher
 * wins. Whether the point is actually *under* that water is the caller's comparison against the
 * ground. See water_field.h for why this is the rule.
 */
inline double water_body_level_at(const MapGraph& graph, const MapCenter& center, const MapPoint& p) {
    std::array<CenterId, 3> cells{};
    std::array<double, 3> weights{};
    if (!graph.triangle_at(center, p, cells, weights)) {
        return center.water_level;
    }
    double level = 0.0;
    for (const CenterId id : cells) {
        level = std::max(level, graph.centers[static_cast<std::size_t>(id)].water_level);
    }
    return level;
}

/** @brief `water_body_level_at()` on a plane map, by coordinates. */
inline double water_body_level_at(const MapGraph& graph, const MapCenter& center, double x, double y) {
    return water_body_level_at(graph, center, MapPoint{x, y});
}

namespace detail {

/**
 * @brief Sideways from a sphere map's segment, along the ground: perpendicular to both the
 *        segment and the globe's radius at its middle, `reach` long. (On a plane it is the 2D
 *        perpendicular, which the plane code below writes out itself.)
 */
inline MapPoint sphere_across(const MapPoint& a, const MapPoint& b, double reach) {
    const double mx = (a.x + b.x) * 0.5, my = (a.y + b.y) * 0.5, mz = (a.z + b.z) * 0.5;
    const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    double cx = my * dz - mz * dy, cy = mz * dx - mx * dz, cz = mx * dy - my * dx;
    double len = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (len <= 0.0) {
        // A zero-length segment: any direction along the ground will do.
        cx = -my; cy = mx; cz = 0.0;
        len = std::sqrt(cx * cx + cy * cy);
        if (len <= 0.0) { cx = 1.0; cy = 0.0; len = 1.0; }
    }
    return MapPoint{cx / len * reach, cy / len * reach, cz / len * reach};
}

/** @brief Along a segment, `reach` long (3D; the zero-length fallback is any sideways step). */
inline MapPoint sphere_along(const MapPoint& a, const MapPoint& b, double reach) {
    const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0.0) return sphere_across(a, b, reach);
    return MapPoint{dx / len * reach, dy / len * reach, dz / len * reach};
}

inline MapPoint offset(const MapPoint& p, const MapPoint& d, double k) {
    return MapPoint{p.x + d.x * k, p.y + d.y * k, p.z + d.z * k};
}

} // namespace detail

namespace detail {

/** @brief A channel segment before it is filed: where it came from and what it drains into. */
struct PlannedChannel {
    ChannelSegment segment;
    CornerId corner = k_invalid_id; /**< @brief The river corner the segment's volume came from. */
    double target = -1.0;           /**< @brief Level of the body the river feeds; < 0 for none. */
    double toward_mouth = 0.0;      /**< @brief 0 upstream, rising to 1 at the mouth. */
    bool mouth = false;             /**< @brief The segment carried on into the body: its water IS the body's. */
    std::vector<CenterId> cells;    /**< @brief Cells to file it under beyond its corner's (a mouth's path). */
};

/** @brief How far a banked river stays under the lower of its two banks, in metres. */
inline constexpr double k_river_bank_margin_m = 0.5;

/**
 * @brief Decides every river's physical surface, then deepens each channel to hold it.
 *
 * Per segment, the water may stand no higher than the lower of its two banks -- the *uncut*
 * ground just past the channel's rim, lowest of three stations along it, less a margin. Taken as
 * a running minimum from the source, the surface then only falls and is under every bank it
 * passes. Near the mouth it backs up to the level of the body it feeds where the banks allow --
 * the estuary is that body's water -- and a backwater carries upstream for as long as the banks
 * hold it. Confluences take the lower of the two rivers, then fall again.
 *
 * Only after that is the cut decided: each segment is made deep enough that its bed lies a
 * river's depth (`river_depth_m` and per volume) under the surface. The default channel is
 * deeper than that almost everywhere, so this deepens only where a low bank upstream has held
 * the water under ground the default cut would have left standing.
 *
 * @param graph The map.
 * @param config Supplies the depths and the scale.
 * @param terrain The detail field the ground is sampled with.
 * @param planned Every river's segments in course order; depths are raised in place.
 * @param out_banked Receives one height per segment of every river.
 */
inline void bank_river_surfaces(const MapGraph& graph, const MapConfig& config, const TerrainDetail& terrain,
                                std::vector<PlannedChannel>& planned,
                                std::vector<std::vector<double>>& out_banked) {
    out_banked.assign(graph.rivers.size(), {});
    std::vector<std::vector<std::size_t>> order(graph.rivers.size());
    for (std::size_t p = 0; p < planned.size(); ++p) {
        const ChannelSegment& segment = planned[p].segment;
        std::vector<double>& profile = out_banked[static_cast<std::size_t>(segment.river)];
        profile.resize(std::max(profile.size(), static_cast<std::size_t>(segment.segment) + 1), 0.0);
        order[static_cast<std::size_t>(segment.river)].push_back(p);
    }
    const double margin = meters_to_height(config, k_river_bank_margin_m);
    const double overlap = meters_to_grid(config, config.water_edge_overlap_m);

    // Per planned segment: its bank cap, and the highest uncut ground along its centreline.
    std::vector<double> caps(planned.size(), 1.0);
    std::vector<double> crowns(planned.size(), 0.0);
    for (std::size_t p = 0; p < planned.size(); ++p) {
        const PlannedChannel& plan = planned[p];
        const ChannelSegment& segment = plan.segment;
        const MapCorner& corner = graph.corners[static_cast<std::size_t>(plan.corner)];
        if (corner.touches.empty()) {
            continue;
        }
        const MapCenter& center = graph.centers[static_cast<std::size_t>(corner.touches.front())];
        if (graph.is_sphere()) {
            const double reach = segment.half_width + overlap;
            const MapPoint across = sphere_across(segment.a, segment.b, reach);
            const MapPoint stations[] = {segment.a,
                                         graph.on_surface(MapPoint{(segment.a.x + segment.b.x) * 0.5,
                                                                   (segment.a.y + segment.b.y) * 0.5,
                                                                   (segment.a.z + segment.b.z) * 0.5}),
                                         segment.b};
            double bank = std::numeric_limits<double>::max();
            double crown = 0.0;
            for (const MapPoint& station : stations) {
                crown = std::max(crown, graph.elevation_at(center, station, terrain));
                for (double side : {-1.0, 1.0}) {
                    const MapPoint q = graph.on_surface(offset(station, across, side));
                    bank = std::min(bank, std::max(graph.elevation_at(center, q, terrain),
                                                   water_body_level_at(graph, center, q) + margin));
                }
            }
            caps[p] = bank - margin;
            crowns[p] = crown;
            continue;
        }
        const double dx = segment.b.x - segment.a.x;
        const double dy = segment.b.y - segment.a.y;
        const double length = std::sqrt(dx * dx + dy * dy);
        const double reach = segment.half_width + overlap;
        const double nx = length > 0.0 ? -dy / length * reach : 0.0;
        const double ny = length > 0.0 ? dx / length * reach : reach;
        const MapPoint stations[] = {segment.a,
                                     {(segment.a.x + segment.b.x) * 0.5, (segment.a.y + segment.b.y) * 0.5},
                                     segment.b};
        double bank = std::numeric_limits<double>::max();
        double crown = 0.0;
        for (const MapPoint& station : stations) {
            crown = std::max(crown, graph.elevation_at(center, station.x, station.y, terrain));
            for (double side : {-1.0, 1.0}) {
                // A bank that is itself open water (the sea at a mouth, a lake shore) holds the
                // river at that water's level, not at the bed under it.
                const double x = station.x + nx * side;
                const double y = station.y + ny * side;
                bank = std::min(bank, std::max(graph.elevation_at(center, x, y, terrain),
                                               water_body_level_at(graph, center, x, y) + margin));
            }
        }
        caps[p] = bank - margin;
        crowns[p] = crown;
    }

    const auto height_of = [&](std::size_t p) -> double& {
        return out_banked[static_cast<std::size_t>(planned[p].segment.river)]
                         [static_cast<std::size_t>(planned[p].segment.segment)];
    };
    const auto fall = [&](std::size_t r) {
        double previous = std::numeric_limits<double>::max();
        for (const std::size_t p : order[r]) {
            double& height = height_of(p);
            height = std::min(height, previous);
            previous = height;
        }
    };

    for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
        if (order[r].empty()) {
            continue;
        }
        for (const std::size_t p : order[r]) {
            height_of(p) = caps[p];
        }
        fall(r);
        // Backed up to the body it feeds, toward the mouth, as far as the banks hold it.
        for (std::size_t k = order[r].size(); k-- > 0;) {
            const std::size_t p = order[r][k];
            const PlannedChannel& plan = planned[p];
            double held = (plan.target >= 0.0 && plan.toward_mouth > 0.0) ? plan.target : -1.0;
            if (k + 1 < order[r].size()) {
                held = std::max(held, height_of(order[r][k + 1]));
            }
            double& height = height_of(p);
            if (held > height) {
                height = std::max(height, std::min(held, caps[p]));
            }
        }
    }

    // Confluences: a corner two rivers share takes the lower of their heights.
    for (int round = 0; round < 2; ++round) {
        std::unordered_map<std::int64_t, double> lowest;
        for (std::size_t p = 0; p < planned.size(); ++p) {
            const std::int64_t key = planned[p].corner;
            auto found = lowest.find(key);
            if (found == lowest.end()) {
                lowest.emplace(key, height_of(p));
            } else {
                found->second = std::min(found->second, height_of(p));
            }
        }
        for (std::size_t p = 0; p < planned.size(); ++p) {
            height_of(p) = std::min(height_of(p), lowest[planned[p].corner]);
        }
        for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
            fall(r);
        }
    }

    // The cut that holds it: the bed a river's depth under the surface, at the least. A mouth
    // stands at its body's level exactly -- it is that body's water, run up the channel.
    for (std::size_t p = 0; p < planned.size(); ++p) {
        PlannedChannel& plan = planned[p];
        double& height = height_of(p);
        if (plan.mouth) height = plan.target;
        height = std::clamp(height, 0.0, 1.0);
        const MapCorner& corner = graph.corners[static_cast<std::size_t>(plan.corner)];
        const double freeboard = meters_to_height(
            config, config.river_depth_m
                        + config.river_depth_per_volume_m * static_cast<double>(std::max(1, corner.river)));
        plan.segment.depth = std::max(plan.segment.depth, crowns[p] - (height - freeboard));
    }
}

} // namespace detail

/**
 * @brief Builds the channel index `MapGraph::elevation_at()` cuts the rivers with.
 *
 * Walks each river's smoothed centreline, pairing every segment with the volume
 * of the corner it came from -- both sequences run source to mouth, so a single
 * proportional step through the corner chain keeps them aligned without a search.
 * Width comes from `river_width()`, the one definition of how wide a river is,
 * so the cut and the ribbon the water layer strokes are the same shape.
 *
 * A segment is filed under the cells its corner *touches*, and that is enough for
 * the surface to stay continuous: a Voronoi corner is shared by three cells and a
 * Voronoi edge by two, so every cell that can see a segment carries it, and both
 * sides of any boundary compute the same cut. It works because the channel is far
 * narrower than a cell -- half a river's width against the 60 m between sites --
 * so nothing further than one cell away is ever within reach of the cut.
 *
 * Build once per render; the sampler takes it by reference and never copies it.
 *
 * @param graph The generated graph; reads `rivers`, `corners` and `centers`.
 * @param config Supplies the channel depths, the river widths and the vertical scale.
 * @return The index, empty when no channel is cut.
 */
inline RiverChannels make_river_channels(const MapGraph& graph, const MapConfig& config,
                                         const TerrainDetail& detail = TerrainDetail{}) {
    RiverChannels channels;
    if (config.river_channel_depth_m <= 0.0 && config.river_channel_depth_per_volume_m <= 0.0) {
        return channels;
    }
    if (graph.rivers.empty() || graph.centers.empty()) {
        return channels;
    }

    const double base = meters_to_height(config, config.river_channel_depth_m);
    const double per_volume = meters_to_height(config, config.river_channel_depth_per_volume_m);
    const double blend_length = meters_to_grid(config, config.river_mouth_blend_m);

    std::vector<detail::PlannedChannel> planned;
    using Planned = detail::PlannedChannel;
    std::vector<std::vector<ChannelSegment>> grouped(graph.centers.size());
    // One stamp per cell, so a segment filed under a cell by two of its three
    // corners is stored once rather than three times. Duplicates would not change
    // the depth -- the sampler takes a max -- but they would lengthen the inner
    // loop for every pixel in the cell.
    std::vector<std::size_t> stamp(graph.centers.size(), static_cast<std::size_t>(-1));
    std::size_t serial = 0;

    for (std::size_t river_index = 0; river_index < graph.rivers.size(); ++river_index) {
        const MapRiver& river = graph.rivers[river_index];
        if (river.points.size() < 2 || river.corners.empty()) {
            continue;
        }
        const std::size_t spans = river.points.size() - 1;

        // Distance back to the mouth, and the level of the water waiting there.
        std::vector<double> to_mouth(spans, 0.0);
        double run = 0.0;
        for (std::size_t i = spans; i-- > 0;) {
            to_mouth[i] = run;
            run += river.points[i].distance_to(river.points[i + 1]);
        }
        const MapCorner& mouth = graph.corners[static_cast<std::size_t>(river.corners.back())];
        double target = 0.0;
        bool has_target = false;
        for (const CenterId center_id : mouth.touches) {
            const MapCenter& center = graph.centers[static_cast<std::size_t>(center_id)];
            if (center.water) {
                target = center.water_level;
                has_target = true;
            }
        }

        for (std::size_t i = 0; i < spans; ++i) {
            const std::size_t slot =
                std::min(river.corners.size() - 1, i * river.corners.size() / spans);
            const MapCorner& corner =
                graph.corners[static_cast<std::size_t>(river.corners[slot])];
            const int volume = std::max(1, corner.river);

            ChannelSegment segment;
            segment.a = river.points[i];
            segment.b = river.points[i + 1];
            segment.half_width = river_width(config, volume) * 0.5;
            segment.depth = base + per_volume * static_cast<double>(volume);
            segment.river = static_cast<std::int32_t>(river_index);
            segment.segment = static_cast<std::int32_t>(i);

            const double toward_mouth =
                blend_length > 0.0 ? std::clamp(1.0 - to_mouth[i] / blend_length, 0.0, 1.0) : 0.0;
            if (has_target && toward_mouth > 0.0 && !corner.touches.empty()) {
                const MapCenter& here =
                    graph.centers[static_cast<std::size_t>(corner.touches.front())];
                const MapPoint middle{(segment.a.x + segment.b.x) * 0.5,
                                      (segment.a.y + segment.b.y) * 0.5,
                                      (segment.a.z + segment.b.z) * 0.5};
                // Deep enough to put the bed at the waterline, and wide enough to
                // carry the whole drawn ribbon -- the probe that decides the water's
                // floor reaches past the stroke diagonally, so anything narrower
                // leaves its edge up on the bank, which is the very thing this is
                // trying to fix.
                // To a freeboard *below* the waterline, not to it. Carved exactly
                // to the target, the bed and the sheet resting on it are the same
                // height, and eight-bit output rounds them into neighbouring greys --
                // terrain showing through the water at the one place this is meant to
                // be seamless. A river's own depth is the natural gap to leave.
                const double freeboard =
                    meters_to_height(config, config.river_depth_m
                                                 + config.river_depth_per_volume_m
                                                       * static_cast<double>(volume));
                const double to_water =
                    graph.elevation_at(here, graph.on_surface(middle), detail) - (target - freeboard);
                const double reach =
                    (river_width(config, volume)
                     + meters_to_grid(config, config.water_edge_overlap_m) * 2.0) * 0.5;
                // Carved *to* the waterline, not merely deepened toward it. Taking
                // the deeper of the two instead dug the bed straight past the sea --
                // the ordinary channel is some 22 m deep and a mouth often needs far
                // less than that, or none at all where the ground already lies below
                // the waterline -- so the sheet resting on the bed ended under the
                // water it was running into, which is the dark notch at every join.
                segment.depth += (std::max(0.0, to_water) - segment.depth) * toward_mouth;
                segment.half_width +=
                    std::max(0.0, reach * k_estuary_reach - segment.half_width) * toward_mouth;
                segment.flat = k_estuary_flat * toward_mouth;
            }

            if (segment.half_width <= 0.0) {
                continue;
            }
            planned.push_back(Planned{segment, river.corners[slot], has_target ? target : -1.0, toward_mouth});
        }

        // The mouth: one more segment, from the river's last point on to the site of the water
        // cell it empties into. A river's course ends on the coastline's corner, but the water
        // starts only where the ground drops under its level -- often a strip of shore further
        // out -- and without this the channel would stop short of it, leaving a dam of dry land
        // between the river and the sea it runs into. Held at the body's level (banked), cut to
        // hold it like any other segment.
        if (has_target) {
            // Run to the nearest cell of that body whose bed is really under its surface: a
            // coastal water cell's bed can stand above the water (its corners are up on the
            // shore), and ending there would leave the dam this exists to cut. Searched a few
            // rings out through the body, the path's cells kept to file the segment under.
            std::vector<CenterId> path;
            std::vector<CenterId> ring;
            for (const CenterId id : mouth.touches) {
                const MapCenter& c = graph.centers[static_cast<std::size_t>(id)];
                if (c.water && c.water_level == target) ring.push_back(id);
            }
            const MapCenter* into = nullptr;
            for (int depth = 0; depth < 4 && !ring.empty() && into == nullptr; ++depth) {
                double nearest = std::numeric_limits<double>::max();
                for (const CenterId id : ring) {
                    const MapCenter& c = graph.centers[static_cast<std::size_t>(id)];
                    const double d = c.point.distance_to(river.points.back());
                    if (c.elevation < c.water_level && d < nearest) {
                        nearest = d;
                        into = &c;
                    }
                }
                path.insert(path.end(), ring.begin(), ring.end());
                std::vector<CenterId> next;
                for (const CenterId id : ring) {
                    for (const CenterId n : graph.centers[static_cast<std::size_t>(id)].neighbors) {
                        const MapCenter& c = graph.centers[static_cast<std::size_t>(n)];
                        if (c.water && c.water_level == target &&
                            std::find(path.begin(), path.end(), n) == path.end() &&
                            std::find(next.begin(), next.end(), n) == next.end()) {
                            next.push_back(n);
                        }
                    }
                }
                ring = std::move(next);
            }
            const int volume = std::max(1, mouth.river);
            ChannelSegment segment;
            segment.a = river.points.back();
            segment.b = into != nullptr ? into->point : river.points.back();
            segment.half_width = river_width(config, volume) * 0.5;
            segment.depth = base + per_volume * static_cast<double>(volume);
            segment.flat = 0.5;
            segment.river = static_cast<std::int32_t>(river_index);
            segment.segment = static_cast<std::int32_t>(spans);
            segment.mouth = true;
            if (segment.a.distance_to(segment.b) > 0.0) {
                Planned plan{segment, river.corners.back(), target, 1.0, true};
                for (const CenterId id : path) {
                    plan.cells.push_back(id);
                    for (const CenterId n : graph.centers[static_cast<std::size_t>(id)].neighbors) plan.cells.push_back(n);
                }
                planned.push_back(std::move(plan));
            }
        }
    }

    // The water surface first, from the uncut ground -- see `RiverChannels::banked` -- then the
    // depth each segment must be cut to so the channel holds it.
    detail::bank_river_surfaces(graph, config, detail, planned, channels.banked);

    for (Planned& plan : planned) {
        ChannelSegment& segment = plan.segment;
        if (segment.depth <= 0.0) {
            continue;
        }
        const MapCorner& corner = graph.corners[static_cast<std::size_t>(plan.corner)];
        ++serial;
        const auto file = [&](CenterId center_id) {
            const std::size_t index = static_cast<std::size_t>(center_id);
            if (index >= grouped.size() || stamp[index] == serial) {
                return;
            }
            stamp[index] = serial;
            grouped[index].push_back(segment);
        };
        for (const CenterId center_id : plan.cells) file(center_id);
        {
            for (const CenterId center_id : corner.touches) {
                file(center_id);
                // An estuary is wide enough to reach ground the corner it came from
                // does not touch, and a cell that does not carry the segment computes
                // no cut for it -- which is a seam down the middle of every mouth.
                // Ordinary channels stay filed under the three cells sharing their
                // corner, which is all their width can reach.
                if (segment.flat > 0.0) {
                    for (const CenterId neighbor_id :
                         graph.centers[static_cast<std::size_t>(center_id)].neighbors) {
                        file(neighbor_id);
                    }
                }
            }
        }
    }

    std::size_t total = 0;
    for (const std::vector<ChannelSegment>& group : grouped) {
        total += group.size();
    }
    channels.segments.reserve(total);
    channels.cells.resize(graph.centers.size());

    for (std::size_t i = 0; i < grouped.size(); ++i) {
        RiverChannels::Cell& cell = channels.cells[i];
        cell.first = channels.segments.size();
        cell.count = grouped[i].size();
        if (cell.count == 0) {
            continue;
        }
        // The box is widened by each segment's own half-width, so a point the box
        // rejects is genuinely outside every cut rather than merely outside the
        // centrelines.
        double min_x = grouped[i].front().a.x;
        double min_y = grouped[i].front().a.y;
        double max_x = min_x;
        double max_y = min_y;
        double min_z = grouped[i].front().a.z;
        double max_z = min_z;
        for (const ChannelSegment& segment : grouped[i]) {
            const double reach = segment.half_width;
            min_x = std::min({min_x, segment.a.x - reach, segment.b.x - reach});
            min_y = std::min({min_y, segment.a.y - reach, segment.b.y - reach});
            max_x = std::max({max_x, segment.a.x + reach, segment.b.x + reach});
            max_y = std::max({max_y, segment.a.y + reach, segment.b.y + reach});
            min_z = std::min({min_z, segment.a.z - reach, segment.b.z - reach});
            max_z = std::max({max_z, segment.a.z + reach, segment.b.z + reach});
            channels.segments.push_back(segment);
        }
        cell.min = MapPoint{min_x, min_y};
        cell.max = MapPoint{max_x, max_y};
        if (graph.is_sphere()) {
            cell.min.z = min_z;
            cell.max.z = max_z;
        }
    }
    return channels;
}

/**
 * @struct RiverSurfaces
 * @brief The drawn water surface of every river, one height per segment.
 *
 * **The one definition of the height a river's water is drawn at**, and it is a
 * whole-network table rather than a function of one segment because none of the
 * three things it has to guarantee are local:
 *
 * - a water surface only ever **falls** -- which needs the segments before it;
 * - it **meets the body it empties into** -- which needs the distance to the mouth;
 * - two rivers meeting **agree on a height** -- which needs the other river.
 *
 * Computed per segment and independently, the surface did none of them: 48 of 55
 * rivers flowed uphill somewhere, the median ocean mouth ended 10.9 m above the
 * sea it ran into, and confluences disagreed by up to 30.8 m.
 *
 * Build once per render, like `RiverChannels` and `TerrainDetail`; the renderer
 * takes it by reference and never recomputes it.
 */
struct RiverSurfaces {
    /** @brief Indexed by river, then by segment; `points.size() - 1` entries each. */
    std::vector<std::vector<double>> heights;

    /**
     * @brief The height a river is drawn at over one segment.
     * @param river Index into `MapGraph::rivers`.
     * @param segment Index into that river's segments.
     * @return The surface height, or 0 when either index is out of range.
     */
    double at(std::size_t river, std::size_t segment) const {
        if (river >= heights.size() || segment >= heights[river].size()) {
            return 0.0;
        }
        return heights[river][segment];
    }
};

namespace detail {

/**
 * @brief The highest drawn ground under one segment's stroke, in height units.
 *
 * Sampled over the whole footprint the renderer paints -- both endpoints stepped
 * past by the reach, the midpoint, and each offset to either side -- because a
 * single sample at the centre can sit under the ground at an end or at the edge of
 * the stroke.
 *
 * @param graph The map being measured.
 * @param river The watercourse.
 * @param segment Which segment of it.
 * @param config Supplies the widths and the vertical scale.
 * @param detail The detail field, so the sheet follows a roughened surface.
 * @param channels The carved channels, or null to sample the ground uncut.
 * @return The highest ground the stroke covers.
 */
inline double river_ground_under(const MapGraph& graph, const MapRiver& river,
                                 std::size_t segment, const MapConfig& config,
                                 const TerrainDetail& detail, const RiverChannels* channels) {
    const std::size_t spans = river.points.size() - 1;
    const std::size_t slot =
        std::min(river.corners.size() - 1, segment * river.corners.size() / spans);
    const MapCorner& corner = graph.corners[static_cast<std::size_t>(river.corners[slot])];
    if (corner.touches.empty()) {
        return 0.0;
    }
    const MapCenter& center = graph.centers[static_cast<std::size_t>(corner.touches.front())];

    const MapPoint& from = river.points[segment];
    const MapPoint& to = river.points[segment + 1];

    // Half the stroke, so the probe reaches the edge of what is actually drawn.
    const double reach =
        (river_width(config, corner.river)
         + meters_to_grid(config, config.water_edge_overlap_m) * 2.0)
        * 0.5;
    if (graph.is_sphere()) {
        const MapPoint along = sphere_along(from, to, reach);
        const MapPoint across = sphere_across(from, to, reach);
        const MapPoint stations[] = {offset(from, along, -1.0), from,
                                     MapPoint{(from.x + to.x) * 0.5, (from.y + to.y) * 0.5, (from.z + to.z) * 0.5},
                                     to, offset(to, along, 1.0)};
        double ground = 0.0;
        for (const MapPoint& station : stations) {
            for (double k = -1.0; k <= 1.0; k += 0.5) {
                const MapPoint q = graph.on_surface(offset(station, across, k));
                ground = std::max(ground, channels == nullptr ? graph.elevation_at(center, q, detail)
                                                              : graph.elevation_at(center, q, detail, *channels));
            }
        }
        return ground;
    }
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double length = std::sqrt(dx * dx + dy * dy);
    // Unit vectors along the segment and across it, scaled to the reach. A zero
    // length segment degenerates to a disc, which the across vector alone covers.
    const double ax = length > 0.0 ? dx / length * reach : reach;
    const double ay = length > 0.0 ? dy / length * reach : 0.0;
    const double nx = length > 0.0 ? -dy / length * reach : 0.0;
    const double ny = length > 0.0 ? dx / length * reach : reach;

    // Five stations along the stroke: past each end, at each end, and the middle.
    // The stepped-past pair cover the rounded caps; the endpoints themselves matter
    // because the channel varies along the course, so ground at the start of a
    // segment is not the ground a reach before it.
    const MapPoint stations[] = {{from.x - ax, from.y - ay},
                                 from,
                                 {(from.x + to.x) * 0.5, (from.y + to.y) * 0.5},
                                 to,
                                 {to.x + ax, to.y + ay}};
    double ground = 0.0;
    for (const MapPoint& station : stations) {
        // Five steps across as well as five along. The renderer paints every pixel
        // of the stroke and this samples a grid over it, so a coarser grid lets a
        // ridge between two probes escape -- and the sheet then settles a hair under
        // it, which at eight-bit output is a whole grey level of terrain showing
        // through the water.
        for (double across = -1.0; across <= 1.0; across += 0.5) {
            const double x = station.x + nx * across;
            const double y = station.y + ny * across;
            ground = std::max(ground, channels == nullptr
                                          ? graph.elevation_at(center, x, y, detail)
                                          : graph.elevation_at(center, x, y, detail, *channels));
        }
    }
    return ground;
}

/** @brief Freeboard of the sheet above the bed over one segment, in height units. */
inline double river_freeboard(const MapGraph& graph, const MapRiver& river, std::size_t segment,
                              const MapConfig& config) {
    const std::size_t spans = river.points.size() - 1;
    const std::size_t slot =
        std::min(river.corners.size() - 1, segment * river.corners.size() / spans);
    const int volume = graph.corners[static_cast<std::size_t>(river.corners[slot])].river;
    return meters_to_height(config, config.river_depth_m
                                        + config.river_depth_per_volume_m
                                              * static_cast<double>(volume));
}

/**
 * @brief Forces one river's profile to fall, and to clear the ground beneath it.
 *
 * Both at once, which is why the floor is taken as a *suffix* maximum first: to be
 * non-increasing and still above the ground everywhere, a segment has to clear not
 * only its own ground but every piece of ground downstream of it. Applying the two
 * rules in turn instead makes them fight -- lowering for monotonicity pushes the
 * sheet into a hillside, raising it off the hillside breaks monotonicity.
 *
 * Holding the water up behind high ground is also the right picture: that is what a
 * natural weir does, rather than the river cutting through it.
 *
 * @param floors The ground plus freeboard each segment must clear.
 * @param surface The profile to settle, in place.
 */
inline std::vector<double> river_binding(const std::vector<double>& floors) {
    std::vector<double> binding(floors.size());
    double running = 0.0;
    for (std::size_t i = floors.size(); i-- > 0;) {
        running = std::max(running, floors[i]);
        binding[i] = running;
    }
    return binding;
}

/**
 * @brief Forces one river's profile to fall, given the bound it may not go under.
 * @param binding The suffix-maximum floor, from `river_binding()`.
 * @param surface The profile to settle, in place.
 */
inline void settle_river_profile(const std::vector<double>& binding,
                                 std::vector<double>& surface) {
    if (surface.empty()) {
        return;
    }
    double previous = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < surface.size(); ++i) {
        surface[i] = std::max(binding[i], std::min(previous, surface[i]));
        previous = surface[i];
    }
}

} // namespace detail

/**
 * @brief Which picture of a river's water `make_river_surfaces()` computes.
 *
 * - **Sheet** -- the 2D water layer's. The ribbon is stroked over the terrain and has to hide the
 *   ground under every pixel of it, so it stands on the *highest* ground under its stroke plus
 *   the river's depth: brim-full, a little proud of its banks. Right for a top-down image, where
 *   nothing beside the ribbon is drawn at a height.
 * - **Banked** -- the physical surface, for anything that meshes the water over the ground in 3D.
 *   Always under *both* banks and only ever falling, with the channel cut to hold it; decided
 *   by `make_river_channels()` and kept in `RiverChannels::banked`. Water that stood proud of
 *   its banks would hang in the air beside them.
 */
enum class RiverFit { Sheet, Banked };


/**
 * @brief Computes the height every river's water is drawn at.
 *
 * Four stages, in order, each fixing something the stage before cannot.
 *
 * **The floor** is the ground under the stroke plus the freeboard, but sampled from
 * two different surfaces and blended between them as the mouth approaches. Upstream
 * it is the *uncut* ground, so the water fills its channel to the rim -- a river
 * brim-full in its valley. At the mouth it is the *cut* ground, the bed itself,
 * because the uncut surface holds the water up on the bank: measured at the coast,
 * the uncut ground sits at 158 m against a sea at 150, while the cut channel is
 * already down at 129. Sampling the rim is what left every river ending above the
 * sea it ran into.
 *
 * **The target** is the surface of the body the river empties into, and the blend
 * toward it is taken with a `min`, so it only ever pulls the river *down*. A river
 * entering a lake whose level stands above it keeps its own height and lets the
 * lake sheet, drawn afterwards, cover its end. Water does not climb.
 *
 * **Settling** makes each profile fall and clear the ground; see
 * `detail::settle_river_profile()`.
 *
 * **Reconciling** gives every corner shared by two rivers one height: the lowest
 * claimed there, but never below the highest ground claimed there. Settling is then
 * re-run, because moving a corner can break the descent that was just established.
 *
 * @param graph The generated graph.
 * @param config Supplies the depths, the widths, the blend length and the scale.
 * @param detail The detail field, so the sheet follows a roughened surface.
 * @param channels The carved channels, so the water can settle into its bed.
 * @param fit Which picture of the water to compute; see `RiverFit`. Everything above describes
 *        `RiverFit::Sheet`, the 2D water layer's.
 * @return One height per segment of every river.
 */
inline RiverSurfaces make_river_surfaces(const MapGraph& graph, const MapConfig& config,
                                         const TerrainDetail& detail,
                                         const RiverChannels& channels,
                                         RiverFit fit = RiverFit::Sheet) {
    if (fit == RiverFit::Banked) {
        RiverSurfaces banked;
        banked.heights = channels.banked;
        banked.heights.resize(graph.rivers.size());
        return banked;
    }
    RiverSurfaces surfaces;
    surfaces.heights.resize(graph.rivers.size());
    const double blend_length = meters_to_grid(config, config.river_mouth_blend_m);

    std::vector<std::vector<double>> floors(graph.rivers.size());
    std::vector<double> targets(graph.rivers.size(), -1.0);
    std::vector<std::vector<double>> mouth_weights(graph.rivers.size());

    for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
        const MapRiver& river = graph.rivers[r];
        if (river.points.size() < 2 || river.corners.empty()) {
            continue;
        }
        const std::size_t spans = river.points.size() - 1;

        // Distance from each segment back to the mouth, for the blend.
        std::vector<double> to_mouth(spans, 0.0);
        double run = 0.0;
        for (std::size_t i = spans; i-- > 0;) {
            to_mouth[i] = run;
            run += river.points[i].distance_to(river.points[i + 1]);
        }

        // The body it empties into, if any.
        const MapCorner& mouth = graph.corners[static_cast<std::size_t>(river.corners.back())];
        double target = 0.0;
        bool has_target = false;
        for (const CenterId center_id : mouth.touches) {
            const MapCenter& center = graph.centers[static_cast<std::size_t>(center_id)];
            if (center.water) {
                target = center.water_level;
                has_target = true;
            }
        }

        floors[r].resize(spans);
        surfaces.heights[r].resize(spans);
        for (std::size_t i = 0; i < spans; ++i) {
            const double freeboard = detail::river_freeboard(graph, river, i, config);
            const double uncut =
                detail::river_ground_under(graph, river, i, config, detail, nullptr);
            const double cut =
                detail::river_ground_under(graph, river, i, config, detail, &channels);
            const double toward_mouth =
                blend_length > 0.0 ? std::clamp(1.0 - to_mouth[i] / blend_length, 0.0, 1.0) : 0.0;

            floors[r][i] = (uncut + (cut - uncut) * toward_mouth) + freeboard;
            double height = floors[r][i];
            if (has_target) {
                // Down only. Where the target stands above the river, this keeps the
                // river's own height.
                height = std::min(height, height + (target - height) * toward_mouth);
            }
            surfaces.heights[r][i] = height;
        }

        // Kept for the final pass, which runs after settling: raising a mouth is
        // exactly the thing monotone descent would undo.
        targets[r] = has_target ? target : -1.0;
        mouth_weights[r] = std::move(to_mouth);
    }

    // The bound each river may not sink below, which is what a confluence has to be
    // reconciled against. Using the raw floor at the shared corner instead leaves the
    // two disagreeing by up to 15 m: settling afterwards lifts whichever river has
    // high ground *downstream* of the corner back above the level just agreed, and no
    // number of rounds converges because the constraint was never the one applied.
    std::vector<std::vector<double>> bindings(graph.rivers.size());
    for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
        bindings[r] = detail::river_binding(floors[r]);
        detail::settle_river_profile(bindings[r], surfaces.heights[r]);
    }

    // Reconcile confluences, then settle again: a corner pulled down by the river it
    // shares can leave the profile around it no longer falling.
    for (int round = 0; round < 2; ++round) {
        std::unordered_map<std::int64_t, double> lowest;
        std::unordered_map<std::int64_t, double> highest_floor;
        const auto visit = [&](auto&& record) {
            for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
                const MapRiver& river = graph.rivers[r];
                if (surfaces.heights[r].empty()) {
                    continue;
                }
                const std::size_t spans = surfaces.heights[r].size();
                for (std::size_t k = 0; k < river.corners.size(); ++k) {
                    const std::size_t segment =
                        std::min(spans - 1, k * spans / std::max<std::size_t>(1, river.corners.size() - 1));
                    record(static_cast<std::int64_t>(river.corners[k]), r, segment);
                }
            }
        };
        visit([&](std::int64_t corner, std::size_t r, std::size_t segment) {
            const double height = surfaces.heights[r][segment];
            auto found = lowest.find(corner);
            if (found == lowest.end()) {
                lowest.emplace(corner, height);
                highest_floor.emplace(corner, bindings[r][segment]);
            } else {
                found->second = std::min(found->second, height);
                auto& floor = highest_floor[corner];
                floor = std::max(floor, bindings[r][segment]);
            }
        });
        visit([&](std::int64_t corner, std::size_t r, std::size_t segment) {
            const double agreed = std::max(lowest[corner], highest_floor[corner]);
            surfaces.heights[r][segment] = agreed;
            // Recorded as a floor, not merely written. Agreeing a confluence can put
            // it *above* what its own river holds upstream, and settling then pulls
            // it straight back down -- which is why simply assigning the level and
            // re-settling never converged. A level a river must hold at a confluence
            // is a level it must hold everywhere above it too: water pools behind an
            // obstruction rather than thinning out over it, and `river_binding()`'s
            // suffix maximum carries exactly that upstream.
            floors[r][segment] = std::max(floors[r][segment], agreed);
        });
        for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
            bindings[r] = detail::river_binding(floors[r]);
            detail::settle_river_profile(bindings[r], surfaces.heights[r]);
        }
    }

    // Last of all: a river ends at the level of what it feeds, never below it.
    //
    // This has to come after settling rather than before, and the reason is the
    // whole difficulty of the join. The blend upstream only ever pulls a river
    // *down*, which is right where a lake stands over it -- water does not climb a
    // hillside. But a mouth that finishes under the sea is the dark notch at every
    // join, and raising it is precisely what monotone descent exists to undo: the
    // settle pass sees a segment higher than the one above it and pulls it back.
    //
    // So the rise is applied at the end and confined to the blend, weighted so the
    // river meets the body exactly at the join and the correction fades out
    // upstream. The profile still falls everywhere else; where a body stands above
    // its own inflow, the last stretch is that body backed up into the valley, which
    // is what a drowned inlet is.
    for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
        if (targets[r] < 0.0 || surfaces.heights[r].empty()) {
            continue;
        }
        for (std::size_t i = 0; i < surfaces.heights[r].size(); ++i) {
            const double toward_mouth =
                blend_length > 0.0
                    ? std::clamp(1.0 - mouth_weights[r][i] / blend_length, 0.0, 1.0)
                    : 0.0;
            double& height = surfaces.heights[r][i];
            height = std::max(height, height + (targets[r] - height) * toward_mouth);
        }
    }

    // Raising a mouth can lift one side of a confluence and not the other, so the
    // shared corners are agreed once more -- this time upward, which is the only
    // direction that keeps every river at or above the body it feeds. No settling
    // follows, because settling is what would undo the rise.
    {
        std::unordered_map<std::int64_t, double> highest;
        const auto visit = [&](auto&& record) {
            for (std::size_t r = 0; r < graph.rivers.size(); ++r) {
                const MapRiver& river = graph.rivers[r];
                const std::size_t spans = surfaces.heights[r].size();
                if (spans == 0) {
                    continue;
                }
                for (std::size_t k = 0; k < river.corners.size(); ++k) {
                    const std::size_t segment = std::min(
                        spans - 1, k * spans / std::max<std::size_t>(1, river.corners.size() - 1));
                    record(static_cast<std::int64_t>(river.corners[k]), r, segment);
                }
            }
        };
        visit([&](std::int64_t corner, std::size_t r, std::size_t segment) {
            auto found = highest.find(corner);
            const double height = surfaces.heights[r][segment];
            if (found == highest.end()) {
                highest.emplace(corner, height);
            } else {
                found->second = std::max(found->second, height);
            }
        });
        visit([&](std::int64_t corner, std::size_t r, std::size_t segment) {
            surfaces.heights[r][segment] = highest[corner];
        });
    }

    for (std::vector<double>& profile : surfaces.heights) {
        for (double& height : profile) {
            height = std::clamp(height, 0.0, 1.0);
        }
    }
    return surfaces;
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_DATA_H
