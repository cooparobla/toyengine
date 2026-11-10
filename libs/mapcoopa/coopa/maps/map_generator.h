/**
 * @file map_generator.h
 * @brief Builds the Voronoi graph from a jittered point grid and drives the
 *        generation passes over it.
 */

#ifndef COOPA_MAPS_MAP_GENERATOR_H
#define COOPA_MAPS_MAP_GENERATOR_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <atomic>
#include <memory>

#include <delaunator/delaunator.hpp>

#include <coopa/job/engine.h>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/passes/pass_biomes.h>
#include <coopa/maps/passes/pass_caves.h>
#include <coopa/maps/passes/pass_coast.h>
#include <coopa/maps/passes/pass_elevation.h>
#include <coopa/maps/passes/pass_landmarks.h>
#include <coopa/maps/passes/pass_moisture.h>
#include <coopa/maps/passes/pass_noisy_edges.h>
#include <coopa/maps/passes/pass_rivers.h>
#include <coopa/maps/passes/pass_temperature.h>
#include <coopa/maps/passes/pass_regions.h>
#include <coopa/maps/passes/pass_roads.h>
#include <coopa/maps/passes/pass_towns.h>
#include <coopa/maps/passes/pass_valleys.h>
#include <coopa/maps/map_task.h>
#include <coopa/maps/passes/pass_water.h>
#include <coopa/maps/portable_random.h>
#include <coopa/maps/portable_sort.h>

namespace coopa {
namespace maps {

/**
 * @class MapGenerator
 * @brief Turns a `MapConfig` into a finished `MapGraph`.
 *
 * The geometry comes first and never changes afterwards: points are laid on a
 * jittered lattice, triangulated, and the Voronoi dual is read off the
 * triangulation's half-edges. Every pass then annotates that fixed graph in
 * turn, each depending on the one before it.
 *
 * The graph is exposed through `graph()` rather than kept private, because the
 * map data -- not the render of it -- is what a game engine consumes.
 */
class MapGenerator {
public:
    /**
     * @brief Constructs a generator.
     * @param config The map description; copied, so the caller may discard it.
     * @param logger Receives one line per stage.
     */
    MapGenerator(const MapConfig& config, coopa::debug::Logger& logger)
        : config_(config), logger_(logger) {}

    /**
     * @brief Generates the map, discarding any previous result.
     * @throws std::runtime_error If the point set is degenerate and cannot be triangulated.
     */
    void generate() { generate_(nullptr, nullptr); }

    /**
     * @brief Sets the engine asynchronous generation runs on; null runs inline.
     *
     * Nothing inside generation is itself threaded, and deliberately so: a
     * default map takes about 135 ms to generate against some 11 s to export, so
     * there is nothing here worth the determinism risk. What the engine buys is
     * getting those 135 ms *off the calling thread*, which is what a frame loop
     * cares about.
     *
     * @param engine Not owned; the house pattern is an injected, nullable engine.
     */
    void set_job_engine(coopa::job::JobEngine* engine) { engine_ = engine; }

    /**
     * @brief Starts generation and returns immediately.
     *
     * The generator itself must outlive the returned task -- the job writes into
     * `graph_`. `MapTask`'s destructor cancels and waits, so declaring the task
     * after the generator and letting both fall out of scope together is correct
     * by construction.
     *
     * Cancellation is checked between passes. Threading a token into each pass
     * would mean changing the single fixed `execute()` signature every pass
     * shares -- the contract `passes/README.md` documents -- to shave at most one
     * pass off the latency, and the longest pass is roughly 50 ms.
     *
     * A cancelled generation leaves `graph()` holding a partly annotated map:
     * every array is intact and internally consistent, but the passes that had
     * not run yet have not written their fields. Treat it as unusable and
     * regenerate.
     *
     * @return A task to poll, measure and cancel.
     */
    MapTask generate_async() {
        auto state = std::make_shared<MapTaskState>();
        state->total.store(k_generation_steps);

        if (engine_ == nullptr) {
            generate_(state, nullptr);
            return MapTask(state);
        }

        const coopa::job::JobHandle handle = engine_->create_handle();
        engine_->submit(
            [this, state](const coopa::job::JobContext& ctx) {
                generate_(state, &ctx);
                state->finish();
            },
            k_map_job_type, handle);
        return MapTask(engine_, handle, state);
    }

    /** @brief The generated map. Empty until `generate()` has been called. */
    const MapGraph& graph() const { return graph_; }

    /** @brief Mutable access to the generated map, for consumers that annotate it further. */
    MapGraph& graph() { return graph_; }

    /** @brief The configuration this generator was built with. */
    const MapConfig& config() const { return config_; }

private:
    MapConfig config_;                /**< @brief The map description being generated. */
    coopa::debug::Logger& logger_;    /**< @brief Progress sink. */
    MapGraph graph_;                  /**< @brief The graph under construction. */
    std::vector<MapPoint> points_;    /**< @brief Generating sites, one per cell. */
    /** @brief Engine `generate_async()` runs on, or null to run inline. Not owned. */
    coopa::job::JobEngine* engine_ = nullptr;

    /** @brief How far outside the grid the boundary ring of points is placed. */
    static constexpr double k_boundary_margin = 1.0;

    /**
     * @brief Lays out the generating sites.
     *
     * A jittered lattice rather than Poisson-disc sampling or Lloyd relaxation:
     * it is one pass, it is trivially reproducible from the seed, and at these
     * densities the resulting cells are close enough to uniform that the
     * difference does not show.
     *
     * A ring of points outside the grid is added so the cells along the map's
     * edge are bounded polygons rather than the unbounded Voronoi regions the
     * hull points would otherwise produce.
     */
    void generate_points_() {
        std::mt19937 rng(static_cast<std::mt19937::result_type>(config_.seed));
        coopa::maps::UniformRealDistribution<double> jitter(-1.0, 1.0);

        points_.clear();
        const int grid_size = config_.grid_size;
        points_.reserve(static_cast<std::size_t>((grid_size + 1) * (grid_size + 1))
                        + static_cast<std::size_t>(4 * (grid_size + 1) + 4));

        for (int x = 0; x <= grid_size; ++x) {
            for (int y = 0; y <= grid_size; ++y) {
                points_.push_back({x + config_.jitter * (jitter(rng) - 0.5),
                                   y + config_.jitter * (jitter(rng) - 0.5)});
            }
        }

        const double margin = k_boundary_margin;
        const double far_edge = grid_size + margin;
        for (int x = 0; x <= grid_size; ++x) {
            points_.push_back({x - margin, -margin});
            points_.push_back({x + margin, far_edge});
        }
        for (int y = 0; y <= grid_size; ++y) {
            points_.push_back({-margin, y - margin});
            points_.push_back({far_edge, y + margin});
        }
        points_.push_back({-margin, -margin});
        points_.push_back({-margin, far_edge});
        points_.push_back({far_edge, -margin});
        points_.push_back({far_edge, far_edge});
    }

    /** @brief Triangulates the point set and reads the Voronoi dual off it. */
    void triangulate_points_() {
        std::vector<double> coords;
        coords.reserve(points_.size() * 2);
        for (const MapPoint& point : points_) {
            coords.push_back(point.x);
            coords.push_back(point.y);
        }

        const delaunator::Delaunator delaunay(coords);
        collect_delaunay_data_(delaunay.triangles, delaunay.halfedges);
    }

    // ------------------------------------------------------------------------
    // Sphere maps (MapTopology::Sphere)
    // ------------------------------------------------------------------------

    /**
     * @brief Lays the generating sites over a globe: a Fibonacci spiral, jittered.
     *
     * The spiral spaces N points almost exactly evenly over the sphere (each claims the same
     * area to within a few percent), which is what the plane's jittered lattice does for the
     * square. The jitter moves each point along the ground by up to `jitter` grid units -- the
     * plane's meaning, since cells here are a grid unit across too.
     */
    void generate_sphere_points_() {
        std::mt19937 rng(static_cast<std::mt19937::result_type>(config_.seed));
        coopa::maps::UniformRealDistribution<double> jitter(-1.0, 1.0);
        const int count = sphere_point_count(config_);
        const double radius = sphere_radius(config_);
        const double golden = 3.14159265358979323846 * (3.0 - std::sqrt(5.0));
        points_.clear();
        points_.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const double z = 1.0 - (2.0 * i + 1.0) / static_cast<double>(count);
            const double r = std::sqrt(std::max(0.0, 1.0 - z * z));
            const double phi = golden * static_cast<double>(i);
            glm::dvec3 dir(r * std::cos(phi), r * std::sin(phi), z);
            // A tangent frame at the point, to jitter along the ground.
            const glm::dvec3 east = glm::length(glm::dvec3(-dir.y, dir.x, 0.0)) > 1e-9
                                        ? glm::normalize(glm::dvec3(-dir.y, dir.x, 0.0))
                                        : glm::dvec3(1.0, 0.0, 0.0);
            const glm::dvec3 north = glm::cross(dir, east);
            const double ju = config_.jitter * (jitter(rng) - 0.5) / radius;
            const double jv = config_.jitter * (jitter(rng) - 0.5) / radius;
            dir = glm::normalize(dir + east * ju + north * jv) * radius;
            points_.push_back({dir.x, dir.y, dir.z});
        }
    }

    /**
     * @brief Triangulates a globe's sites: a spherical Delaunay triangulation (its convex hull).
     *
     * Through the plane triangulator, by stereographic projection: projecting the sphere from
     * one site onto a plane maps circles to circles, so the plane's Delaunay triangulation of the
     * other sites is exactly the sphere's -- less the triangles round the projection point, which
     * went to infinity. Those are put back as a fan from that site to every edge of the plane
     * triangulation's hull. The result is closed: every edge has two sides, every corner three
     * cells, and `collect_delaunay_data_()` reads it exactly as it reads a plane's.
     */
    void triangulate_sphere_points_() {
        const std::size_t count = points_.size();
        const std::size_t pole = 0;
        const glm::dvec3 n = glm::normalize(glm::dvec3(points_[pole].x, points_[pole].y, points_[pole].z));
        const glm::dvec3 helper = std::abs(n.z) < 0.9 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(1.0, 0.0, 0.0);
        const glm::dvec3 u = glm::normalize(glm::cross(helper, n));
        const glm::dvec3 v = glm::cross(n, u);

        std::vector<double> coords;
        std::vector<std::size_t> original;   // projected index -> site index
        coords.reserve(count * 2);
        original.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (i == pole) continue;
            const glm::dvec3 q = glm::normalize(glm::dvec3(points_[i].x, points_[i].y, points_[i].z));
            const double denom = 1.0 - glm::dot(q, n);
            if (denom <= 1e-12) continue;   // coincides with the pole: dropped like a duplicate
            coords.push_back(glm::dot(q, u) / denom);
            coords.push_back(glm::dot(q, v) / denom);
            original.push_back(i);
        }
        const delaunator::Delaunator delaunay(coords);

        std::vector<std::size_t> triangles;
        triangles.reserve(delaunay.triangles.size() + 3 * 256);
        for (const std::size_t t : delaunay.triangles) triangles.push_back(original[t]);
        // The fan: one triangle per hull half-edge a->b, wound b->a->pole so it pairs.
        for (std::size_t e = 0; e < delaunay.halfedges.size(); ++e) {
            if (delaunay.halfedges[e] != delaunator::INVALID_INDEX) continue;
            const std::size_t next = (e % 3 == 2) ? e - 2 : e + 1;
            const std::size_t a = original[delaunay.triangles[e]];
            const std::size_t b = original[delaunay.triangles[next]];
            triangles.push_back(b);
            triangles.push_back(a);
            triangles.push_back(pole);
        }
        // Pair every half-edge with its opposite.
        std::vector<std::size_t> halfedges(triangles.size(), delaunator::INVALID_INDEX);
        std::unordered_map<std::uint64_t, std::size_t> open;
        open.reserve(triangles.size());
        for (std::size_t e = 0; e < triangles.size(); ++e) {
            const std::size_t next = (e % 3 == 2) ? e - 2 : e + 1;
            const std::uint64_t a = triangles[e];
            const std::uint64_t b = triangles[next];
            const auto found = open.find((b << 32) | a);
            if (found != open.end()) {
                halfedges[e] = found->second;
                halfedges[found->second] = e;
                open.erase(found);
            } else {
                open.emplace((a << 32) | b, e);
            }
        }
        collect_delaunay_data_(triangles, halfedges);
    }

    /** @brief A sphere map's Voronoi vertex: the outward normal of its Delaunay triangle, on the globe. */
    MapPoint sphere_corner_(const MapPoint& a, const MapPoint& b, const MapPoint& c) const {
        const glm::dvec3 pa(a.x, a.y, a.z), pb(b.x, b.y, b.z), pc(c.x, c.y, c.z);
        glm::dvec3 nrm = glm::cross(pb - pa, pc - pa);
        const double len = glm::length(nrm);
        if (len <= 0.0) nrm = pa + pb + pc;
        if (glm::dot(nrm, pa + pb + pc) < 0.0) nrm = -nrm;
        nrm = glm::normalize(nrm) * graph_.sphere_radius;
        return {nrm.x, nrm.y, nrm.z};
    }

    /** @brief Sorts each cell's corners counter-clockwise about its site, seen from outside the globe. */
    void sort_sphere_cell_corners_() {
        for (MapCenter& center : graph_.centers) {
            if (center.corners.size() < 3) continue;
            const glm::dvec3 nrm = glm::normalize(glm::dvec3(center.point.x, center.point.y, center.point.z));
            const glm::dvec3 helper = std::abs(nrm.z) < 0.9 ? glm::dvec3(0.0, 0.0, 1.0) : glm::dvec3(1.0, 0.0, 0.0);
            const glm::dvec3 e1 = glm::normalize(glm::cross(helper, nrm));
            const glm::dvec3 e2 = glm::cross(nrm, e1);
            const auto angle = [&](CornerId id) {
                const MapPoint& p = graph_.corners[static_cast<std::size_t>(id)].point;
                const glm::dvec3 d = glm::dvec3(p.x, p.y, p.z) - glm::dvec3(center.point.x, center.point.y, center.point.z);
                return std::atan2(glm::dot(d, e2), glm::dot(d, e1));
            };
            coopa::maps::sort(center.corners.begin(), center.corners.end(),
                              [&](CornerId a, CornerId b) { return angle(a) < angle(b); });
        }
    }

    /** @brief A globe's border: wherever its landmass shape (`SphereShapeField`) keeps the land away. */
    void sphere_border_check_() {
        const double border = config_.border_length;
        const SphereShapeField shape(config_);
        for (MapCenter& center : graph_.centers) {
            if (shape.inset(center.point.x, center.point.y, center.point.z) <= border) center.border = true;
            for (const CornerId corner_id : center.corners) {
                MapCorner& corner = graph_.corners[static_cast<std::size_t>(corner_id)];
                if (shape.inset(corner.point.x, corner.point.y, corner.point.z) <= border) {
                    corner.border = true;
                    center.border = true;
                }
            }
        }
    }

    /**
     * @brief Builds cells, corners and edges from the triangulation.
     *
     * One cell per input point, one corner per triangle circumcentre, and one
     * edge per pair of adjacent triangles. Each interior edge is visited from
     * both of its half-edges; only the lower index creates the edge, so the
     * arrays hold one record per geometric edge rather than the two the
     * original produced.
     */
    void collect_delaunay_data_(const std::vector<std::size_t>& triangles,
                                const std::vector<std::size_t>& halfedges) {
        graph_.centers.resize(points_.size());
        for (std::size_t i = 0; i < points_.size(); ++i) {
            graph_.centers[i].index = static_cast<CenterId>(i);
            graph_.centers[i].point = points_[i];
        }

        const std::size_t triangle_count = triangles.size() / 3;
        graph_.corners.resize(triangle_count);
        for (std::size_t t = 0; t < triangle_count; ++t) {
            const std::size_t i0 = triangles[t * 3];
            const std::size_t i1 = triangles[t * 3 + 1];
            const std::size_t i2 = triangles[t * 3 + 2];

            MapCorner& corner = graph_.corners[t];
            corner.index = static_cast<CornerId>(t);
            corner.point = graph_.is_sphere() ? sphere_corner_(points_[i0], points_[i1], points_[i2])
                                              : circumcenter_(points_[i0], points_[i1], points_[i2]);

            const CornerId corner_id = corner.index;
            for (const std::size_t center_index : {i0, i1, i2}) {
                graph_.centers[center_index].corners.push_back(corner_id);
                corner.touches.push_back(static_cast<CenterId>(center_index));
            }
        }

        for (std::size_t e = 0; e < halfedges.size(); ++e) {
            const std::size_t paired = halfedges[e];
            if (paired == delaunator::INVALID_INDEX || paired < e) {
                continue; // Hull edge, or the far side of an edge already built.
            }

            const CenterId d0 = static_cast<CenterId>(triangles[e]);
            const CenterId d1 = static_cast<CenterId>(triangles[paired]);
            const CornerId v0 = static_cast<CornerId>(e / 3);
            const CornerId v1 = static_cast<CornerId>(paired / 3);

            MapEdge edge;
            edge.index = static_cast<EdgeId>(graph_.edges.size());
            edge.d0 = d0;
            edge.d1 = d1;
            edge.v0 = v0;
            edge.v1 = v1;
            edge.midpoint = {(graph_.corners[static_cast<std::size_t>(v0)].point.x
                              + graph_.corners[static_cast<std::size_t>(v1)].point.x) * 0.5,
                             (graph_.corners[static_cast<std::size_t>(v0)].point.y
                              + graph_.corners[static_cast<std::size_t>(v1)].point.y) * 0.5};
            if (graph_.is_sphere()) {
                edge.midpoint.z = (graph_.corners[static_cast<std::size_t>(v0)].point.z
                                   + graph_.corners[static_cast<std::size_t>(v1)].point.z) * 0.5;
                edge.midpoint = graph_.on_surface(edge.midpoint);
            }

            const EdgeId edge_id = edge.index;
            graph_.edges.push_back(edge);

            graph_.centers[static_cast<std::size_t>(d0)].borders.push_back(edge_id);
            graph_.centers[static_cast<std::size_t>(d1)].borders.push_back(edge_id);
            graph_.centers[static_cast<std::size_t>(d0)].neighbors.push_back(d1);
            graph_.centers[static_cast<std::size_t>(d1)].neighbors.push_back(d0);

            graph_.corners[static_cast<std::size_t>(v0)].protrudes.push_back(edge_id);
            graph_.corners[static_cast<std::size_t>(v1)].protrudes.push_back(edge_id);
            graph_.corners[static_cast<std::size_t>(v0)].adjacent.push_back(v1);
            graph_.corners[static_cast<std::size_t>(v1)].adjacent.push_back(v0);
        }
    }

    /**
     * @brief Sorts each cell's corners counter-clockwise about its centroid.
     *
     * Corners arrive in triangulation order, which is arbitrary; fan-filling or
     * containment-testing an arbitrarily ordered polygon produces nonsense. The
     * original had this call commented out and re-sorted inside each renderer
     * instead, which left `MapCenter::corners` unusable for anything else.
     */
    void sort_cell_corners_() {
        for (MapCenter& center : graph_.centers) {
            if (center.corners.size() < 3) {
                continue;
            }
            const MapPoint centroid = centroid_of_(center.corners);
            coopa::maps::sort(center.corners.begin(), center.corners.end(),
                      [this, centroid](CornerId a, CornerId b) {
                          const MapPoint& pa = graph_.corners[static_cast<std::size_t>(a)].point;
                          const MapPoint& pb = graph_.corners[static_cast<std::size_t>(b)].point;
                          return std::atan2(pa.y - centroid.y, pa.x - centroid.x)
                               < std::atan2(pb.y - centroid.y, pb.x - centroid.x);
                      });
        }
    }

    /** @brief Mean position of a set of corners. */
    MapPoint centroid_of_(const std::vector<CornerId>& corner_ids) const {
        double sum_x = 0.0;
        double sum_y = 0.0;
        for (const CornerId corner_id : corner_ids) {
            sum_x += graph_.corners[static_cast<std::size_t>(corner_id)].point.x;
            sum_y += graph_.corners[static_cast<std::size_t>(corner_id)].point.y;
        }
        const double count = static_cast<double>(corner_ids.size());
        return {sum_x / count, sum_y / count};
    }

    /**
     * @brief Flags the band of cells and corners at the edge of the map.
     *
     * Bordering cells are forced to water by the water pass, which is what
     * guarantees a map is an island rather than a landmass sliced off by the
     * frame. It marks every offending corner of a cell, not just the first one
     * found.
     *
     * The frame is `MapConfig::shape` -- a rectangle spanning the canvas by
     * default, or any other outline inscribed in it.
     * See `ShapeField`.
     */
    void border_check_() {
        const double border = config_.border_length;
        // One field for the whole sweep. The organic shapes resolve a landmass
        // layout and a noise generator at construction, and this runs once per
        // cell and again per corner -- rebuilding that per query, which is what
        // the free `shape_inset()` does, would dominate generation.
        const ShapeField shape(config_);

        for (MapCenter& center : graph_.centers) {
            // The cell's own site, first, and not only for tidiness. The boundary
            // ring emits each outer corner point more than once -- once from the
            // x loop, once from the y loop, once explicitly -- and Delaunator
            // collapses the duplicates into cells with no corners, no edges and no
            // neighbours at all. A loop over `corners` never executes for those,
            // so they came out flagged neither border nor water: dry land sitting
            // outside the map with no geometry. Harmless only by luck, since their
            // default `Ocean` biome scores zero habitability and kept the town and
            // road passes from ever choosing one.
            if (shape.inset(center.point.x, center.point.y) <= border) {
                center.border = true;
            }

            for (const CornerId corner_id : center.corners) {
                MapCorner& corner = graph_.corners[static_cast<std::size_t>(corner_id)];
                // One call decides the shape of the world. Everything downstream
                // reads this flag and nothing downstream knows what shape it is:
                // the water pass floods it, the elevation pass measures height
                // outward from it, and coastlines, regions and roads follow.
                if (shape.inset(corner.point.x, corner.point.y) <= border) {
                    corner.border = true;
                    center.border = true;
                }
            }
        }
    }

    /** @brief Runs every enabled pass, in dependency order. */
    /**
     * @brief The one generation implementation, reached synchronously or on a job.
     *
     * `generate()` and `generate_async()` differ only in what they pass here, so
     * the two cannot drift apart -- the same reasoning behind
     * `AnimationSystem`'s shared serial/parallel body.
     *
     * @param state Optional progress counter, stepped once per stage.
     * @param ctx Optional job context, polled for cancellation between passes.
     */
    void generate_(const std::shared_ptr<MapTaskState>& state,
                   const coopa::job::JobContext* ctx) {
        logger_.info("map generation: start (seed " + std::to_string(config_.seed)
                     + ", grid " + std::to_string(config_.grid_size) + ")");

        graph_.clear();
        if (config_.topology == MapTopology::Sphere) {
            graph_.sphere_radius = sphere_radius(config_);
            generate_sphere_points_();
            triangulate_sphere_points_();
            sort_sphere_cell_corners_();
            sphere_border_check_();
        } else {
        generate_points_();
        triangulate_points_();
        sort_cell_corners_();
        border_check_();
        }
        if (state) {
            state->step();   // The geometry build is one stage of the fifteen.
        }
        if (!execute_passes_(state, ctx)) {
            logger_.info("map generation: cancelled");
            return;
        }

        logger_.info("map generation: done (" + std::to_string(graph_.centers.size())
                     + " cells, " + std::to_string(graph_.corners.size())
                     + " corners, " + std::to_string(graph_.edges.size()) + " edges)");
    }

    /** @brief Steps `generate_()` reports: the geometry build plus the fourteen passes. */
    static constexpr int k_generation_steps = 15;

    /**
     * @brief Runs one pass, counting it and checking for cancellation first.
     *
     * A disabled pass still counts as a step, so progress tracks how far through
     * the pipeline the run is rather than how much work it happened to need --
     * which is what a loading bar wants.
     *
     * @tparam Pass The pass type to run.
     * @param enabled Its `MapConfig::enable_*` toggle.
     * @param state Optional progress counter.
     * @param ctx Optional job context, polled for cancellation.
     * @return False if the run has been cancelled and should stop.
     */
    template <typename Pass>
    bool stage_(bool enabled, const std::shared_ptr<MapTaskState>& state,
                const coopa::job::JobContext* ctx) {
        if (ctx != nullptr && ctx->is_cancelled()) {
            return false;
        }
        if (enabled) {
            Pass{}.execute(graph_, config_, logger_);
        }
        if (state) {
            state->step();
        }
        return true;
    }

    /**
     * @brief Runs every pass in order, stopping early if cancelled.
     *
     * The order is a dependency chain, not a preference -- valleys need rivers,
     * moisture needs rivers, biomes need moisture, towns need regions. Valleys
     * runs early, before moisture rather than last, so that everything reading
     * the height field afterwards reads the carved one: roads route down the
     * valleys, towns settle in them, and biomes classify the floors. Chained with `&&` so that a
     * cancellation short-circuits the rest without an early-return ladder.
     *
     * @param state Optional progress counter.
     * @param ctx Optional job context, polled for cancellation between passes.
     * @return False if the run was abandoned partway.
     */
    bool execute_passes_(const std::shared_ptr<MapTaskState>& state = nullptr,
                         const coopa::job::JobContext* ctx = nullptr) {
        return stage_<PassWater>(config_.enable_water, state, ctx)
            && stage_<PassCoast>(config_.enable_coast, state, ctx)
            && stage_<PassElevation>(config_.enable_elevation, state, ctx)
            && stage_<PassTemperature>(config_.enable_temperature, state, ctx)
            && stage_<PassRivers>(config_.enable_rivers, state, ctx)
            && stage_<PassValleys>(config_.enable_valleys, state, ctx)
            && stage_<PassMoisture>(config_.enable_moisture, state, ctx)
            && stage_<PassBiomes>(config_.enable_biomes, state, ctx)
            && stage_<PassRoads>(config_.enable_roads, state, ctx)
            && stage_<PassRegions>(config_.enable_regions, state, ctx)
            && stage_<PassTowns>(config_.enable_towns, state, ctx)
            && stage_<PassLandmarks>(config_.enable_landmarks, state, ctx)
            && stage_<PassCaves>(config_.enable_caves, state, ctx)
            && stage_<PassNoisyEdges>(config_.enable_noisy_edges, state, ctx);
    }

    /**
     * @brief Circumcentre of a triangle, which is the Voronoi vertex of its three cells.
     *
     * Falls back to the centroid when the three points are collinear. The
     * original divided by the zero determinant unguarded, seeding the graph
     * with NaN corners that then propagated through every pass.
     */
    static MapPoint circumcenter_(const MapPoint& a, const MapPoint& b, const MapPoint& c) {
        const double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
        if (d == 0.0 || !std::isfinite(d)) {
            return {(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0};
        }

        const double a2 = a.x * a.x + a.y * a.y;
        const double b2 = b.x * b.x + b.y * b.y;
        const double c2 = c.x * c.x + c.y * c.y;
        return {(a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d,
                (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d};
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_GENERATOR_H
