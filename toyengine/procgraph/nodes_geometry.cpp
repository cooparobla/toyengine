/**
 * @file nodes_geometry.cpp
 * @brief Operations on geometry: transform, join, instance, and the mesh/point conversions.
 *
 * Most of these are `Cheap` and fuse into their neighbours' chain; the ones that touch every
 * vertex of a potentially large mesh are `Parallel` and use `EvalContext::jobs()` when it is
 * available, falling back to a plain loop when it is not. That dual path is the contract the
 * library sets out: a null engine means "do the work here, spawn nothing".
 */

#include <toyengine/procgraph/nodes.h>

#include <algorithm>
#include <cmath>

#include <coopa/job/engine.h>
#include <coopa/job/parallel_for.h>
#include <glm/gtc/matrix_transform.hpp>

namespace toy {
namespace procgraph {
namespace {

/// @brief Runs `body(begin, end)` over [0, n), on the job engine when there is one.
template <typename Body>
void for_range(EvalContext& ctx, size_t n, size_t grain, Body&& body) {
    if (n == 0) return;
    // Below the grain there is nothing to win from scheduling, and a tiny parallel_for costs more
    // than the loop it replaces.
    if (auto* jobs = ctx.jobs(); jobs != nullptr && n >= grain * 2) {
        jobs->parallel_for_blocking(n, grain, [&body](size_t begin, size_t end) { body(begin, end); },
                                    0, coopa::job::Priority::Low, coopa::job::Priority::Low);
    } else {
        body(size_t(0), n);
    }
}

/// @brief The inverse-transpose, for taking normals through a non-uniform scale correctly.
glm::mat3 normal_matrix(const glm::mat4& m) {
    return glm::transpose(glm::inverse(glm::mat3(m)));
}

void transform_mesh(MeshGeometry& mesh, const glm::mat4& m, EvalContext& ctx) {
    const glm::mat3 nm = normal_matrix(m);
    // A mirroring transform flips winding; leaving it alone would turn the mesh inside out.
    const bool flips = glm::determinant(glm::mat3(m)) < 0.0f;
    for_range(ctx, mesh.vertices.size(), 2048, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            Vertex& v = mesh.vertices[i];
            v.position = glm::vec3(m * glm::vec4(v.position, 1.0f));
            v.normal   = glm::normalize(nm * v.normal);
            const glm::vec3 t = glm::normalize(glm::mat3(m) * glm::vec3(v.tangent));
            v.tangent  = glm::vec4(t, v.tangent.w);
        }
    });
    if (flips) {
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            std::swap(mesh.indices[t + 1], mesh.indices[t + 2]);
        }
    }
}

void transform_points(PointCloud& points, const glm::mat4& m, EvalContext& ctx) {
    const glm::quat rotation = glm::quat_cast(glm::mat3(m));
    const bool had_rotations = !points.rotations.empty();
    if (!had_rotations) points.rotations.assign(points.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    for_range(ctx, points.size(), 4096, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            points.positions[i] = glm::vec3(m * glm::vec4(points.positions[i], 1.0f));
            points.rotations[i] = rotation * points.rotations[i];
        }
    });
}

} // namespace

void register_geometry_nodes() {
    using coopa::nodegraph::register_node_type;

    register_node_type(NodeType{
        "TransformGeometry", "Geometry", "Transform Geometry", {}, NodeCost::Parallel,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("geometry", types().geometry_t);
            b.input_default("translation", types().vector_t, glm::vec3(0.0f));
            b.input_default("rotation", types().rotation_t, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            b.input_default("scale", types().vector_t, glm::vec3(1.0f));
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            GeometrySet geo = detail::geometry_in(ctx, "geometry");
            if (geo.empty()) {
                ctx.set_output("geometry", make_geometry(geo));
                return;
            }
            glm::mat4 m = glm::translate(glm::mat4(1.0f),
                                         ctx.get<glm::vec3>("translation", glm::vec3(0.0f)));
            m *= glm::mat4_cast(ctx.get<glm::quat>("rotation", glm::quat(1.0f, 0.0f, 0.0f, 0.0f)));
            m = glm::scale(m, ctx.get<glm::vec3>("scale", glm::vec3(1.0f)));

            // mutable_data() clones only if the input is genuinely shared, so a node at the end of
            // a linear chain usually transforms in place.
            auto& data = geo.mutable_data();
            if (data.mesh.has_value()) transform_mesh(*data.mesh, m, ctx);
            if (data.points.has_value()) transform_points(*data.points, m, ctx);
            for (auto& group : data.instances) {
                for (auto& t : group.transforms) t = m * t;
            }
            ctx.set_output("geometry", make_geometry(std::move(geo)));
        }});

    register_node_type(NodeType{
        "JoinGeometry", "Geometry", "Join Geometry", "Merges any number of inputs into one",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.multi_input("geometry", types().geometry_t);
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            const auto& in = ctx.input("geometry");
            GeometrySet out;
            for (uint32_t i = 0; i < in.count; ++i) {
                out.merge(in.at(i).as<GeometrySet>(empty_geometry()));
            }
            ctx.set_output("geometry", make_geometry(std::move(out)));
        }});

    register_node_type(NodeType{
        "SetMaterial", "Geometry", "Set Material",
        "Assigns a material to every triangle of a mesh", NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("geometry", types().geometry_t);
            b.input_default("material", types().string_t, std::string());
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            GeometrySet geo = detail::geometry_in(ctx, "geometry");
            const std::string material = ctx.get<std::string>("material", std::string());
            if (material.empty() || !geo.has_mesh()) {
                ctx.set_output("geometry", make_geometry(geo));
                return;
            }
            auto& data = geo.mutable_data();
            MeshGeometry& mesh = *data.mesh;
            // Every triangle moves to one slot named for the material. Join Geometry merges slots
            // by NAME, so two differently-materialled meshes joined afterwards keep their own.
            mesh.slot_names.assign(1, material);
            mesh.face_slots.assign(mesh.triangle_count(), 0u);
            ctx.set_output("geometry", make_geometry(std::move(geo)));
        }});

    register_node_type(NodeType{
        "MeshToPoints", "Geometry", "Mesh to Points",
        "One point per vertex, so a mesh can drive instancing", NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("geometry", types().geometry_t);
            b.output("points", types().geometry_t);
            b.output("count", types().int_t);
        },
        [](EvalContext& ctx) {
            const GeometrySet geo = detail::geometry_in(ctx, "geometry");
            PointCloud cloud;
            if (const MeshGeometry* m = geo.mesh()) {
                cloud.positions.reserve(m->vertices.size());
                for (const auto& v : m->vertices) cloud.positions.push_back(v.position);
            }
            const auto count = static_cast<int32_t>(cloud.positions.size());
            ctx.set_output("points", make_geometry(GeometrySet::from_points(std::move(cloud))));
            ctx.set_output("count", make_int(count));
        }});

    register_node_type(NodeType{
        "SetPosition", "Geometry", "Set Position",
        "Offsets every point or vertex by one vector", NodeCost::Parallel,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("geometry", types().geometry_t);
            b.input_default("offset", types().vector_t, glm::vec3(0.0f));
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            // A single uniform offset, not a per-element field: fields are the library's stage-3
            // work (see coopa/nodegraph/README.md's Limits). Once they land this node gains a
            // per-element `offset` and a `selection` without its signature changing shape.
            GeometrySet geo = detail::geometry_in(ctx, "geometry");
            const glm::vec3 offset = ctx.get<glm::vec3>("offset", glm::vec3(0.0f));
            if (geo.empty() || offset == glm::vec3(0.0f)) {
                ctx.set_output("geometry", make_geometry(geo));
                return;
            }
            auto& data = geo.mutable_data();
            if (data.mesh.has_value()) {
                MeshGeometry& m = *data.mesh;
                for_range(ctx, m.vertices.size(), 4096, [&](size_t begin, size_t end) {
                    for (size_t i = begin; i < end; ++i) m.vertices[i].position += offset;
                });
            }
            if (data.points.has_value()) {
                PointCloud& p = *data.points;
                for_range(ctx, p.size(), 8192, [&](size_t begin, size_t end) {
                    for (size_t i = begin; i < end; ++i) p.positions[i] += offset;
                });
            }
            ctx.set_output("geometry", make_geometry(std::move(geo)));
        }});

    register_node_type(NodeType{
        "FlipFaces", "Geometry", "Flip Faces", {}, NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("geometry", types().geometry_t);
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            GeometrySet geo = detail::geometry_in(ctx, "geometry");
            if (!geo.has_mesh()) {
                ctx.set_output("geometry", make_geometry(geo));
                return;
            }
            auto& data = geo.mutable_data();
            MeshGeometry& m = *data.mesh;
            for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
                std::swap(m.indices[t + 1], m.indices[t + 2]);
            }
            for (auto& v : m.vertices) v.normal = -v.normal;
            ctx.set_output("geometry", make_geometry(std::move(geo)));
        }});

    register_node_type(NodeType{
        "RecomputeNormals", "Geometry", "Recompute Normals",
        "Area-weighted vertex normals, for geometry assembled from parts", NodeCost::Heavy,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("geometry", types().geometry_t);
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            GeometrySet geo = detail::geometry_in(ctx, "geometry");
            if (!geo.has_mesh()) {
                ctx.set_output("geometry", make_geometry(geo));
                return;
            }
            geo.mutable_data().mesh->recompute_normals();
            ctx.set_output("geometry", make_geometry(std::move(geo)));
        }});

    register_node_type(NodeType{
        "BoundingBox", "Geometry", "Bounding Box", {}, NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("geometry", types().geometry_t);
            b.output("min", types().vector_t);
            b.output("max", types().vector_t);
            b.output("size", types().vector_t);
            b.output("center", types().vector_t);
        },
        [](EvalContext& ctx) {
            const GeometrySet geo = detail::geometry_in(ctx, "geometry");
            glm::vec3 lo(0.0f);
            glm::vec3 hi(0.0f);
            bool first = true;
            auto fold = [&](const glm::vec3& p) {
                if (first) { lo = hi = p; first = false; }
                else { lo = glm::min(lo, p); hi = glm::max(hi, p); }
            };
            if (const MeshGeometry* m = geo.mesh()) {
                for (const auto& v : m->vertices) fold(v.position);
            }
            if (const PointCloud* p = geo.points()) {
                for (const auto& pos : p->positions) fold(pos);
            }
            ctx.set_output("min", make_vector(lo));
            ctx.set_output("max", make_vector(hi));
            ctx.set_output("size", make_vector(hi - lo));
            ctx.set_output("center", make_vector((lo + hi) * 0.5f));
        }});

    register_node_type(NodeType{
        "InstanceOnPoints", "Instances", "Instance on Points",
        "Places a prototype at every point. `kind: mesh` in its props names a bare mesh asset "
        "instead of an object asset",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("points", types().geometry_t);
            b.input_default("prototype", types().string_t, std::string());
            b.input_default("rotation", types().rotation_t, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            b.input_default("scale", types().vector_t, glm::vec3(1.0f));
            b.input_default("random_rotation", types().float_t, 0.0f);
            b.input_default("random_scale", types().float_t, 0.0f);
            b.output("instances", types().geometry_t);
            b.output("count", types().int_t);
        },
        [](EvalContext& ctx) {
            const GeometrySet in = detail::geometry_in(ctx, "points");
            const std::string prototype = ctx.get<std::string>("prototype", std::string());
            if (prototype.empty()) {
                ctx.error("Instance on Points needs a `prototype` (an object or mesh asset name)");
                ctx.set_output("count", make_int(0));
                return;
            }
            const PointCloud* points = in.points();
            if (points == nullptr || points->empty()) {
                ctx.set_output("instances", make_geometry(GeometrySet()));
                ctx.set_output("count", make_int(0));
                return;
            }

            const glm::quat base_rot = ctx.get<glm::quat>("rotation", glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            const glm::vec3 base_scale = ctx.get<glm::vec3>("scale", glm::vec3(1.0f));
            const float rand_rot = ctx.get<float>("random_rotation", 0.0f);
            const float rand_scale = std::clamp(ctx.get<float>("random_scale", 0.0f), 0.0f, 1.0f);
            const uint32_t seed = bake_context(ctx).seed;

            InstanceGroup group;
            group.prototype = prototype;
            group.prototype_is_mesh = ctx.props().str("kind", "object") == "mesh";
            group.transforms.reserve(points->size());
            group.seeds.reserve(points->size());
            for (size_t i = 0; i < points->size(); ++i) {
                const auto index = static_cast<uint32_t>(i);
                // Every random draw is keyed on (bake seed, point index) and nothing else, so the
                // same graph and seed produce byte-identical instances every run and on every
                // thread -- which is what makes a bake comparable between the two evaluators.
                const float spin = rand_rot * (detail::hash01(seed, index) * 2.0f - 1.0f);
                const float jitter = 1.0f + rand_scale * (detail::hash01(seed ^ 0x5bf03635u, index) * 2.0f - 1.0f);

                glm::mat4 m = glm::translate(glm::mat4(1.0f), points->positions[i]);
                m *= glm::mat4_cast(points->rotation_of(i) * base_rot *
                                    glm::angleAxis(spin, glm::vec3(0.0f, 0.0f, 1.0f)));
                m = glm::scale(m, points->scale_of(i) * base_scale * jitter);
                group.transforms.push_back(m);
                group.seeds.push_back(detail::hash_u32(seed ^ index));
            }
            const auto count = static_cast<int32_t>(group.transforms.size());
            ctx.set_output("instances", make_geometry(GeometrySet::from_instances(std::move(group))));
            ctx.set_output("count", make_int(count));
        }});

    register_node_type(NodeType{
        "PointsToVertices", "Geometry", "Points to Vertices",
        "Treats a point cloud as loose vertices, for a graph that wants bounds or a count",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("points", types().geometry_t);
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            const GeometrySet in = detail::geometry_in(ctx, "points");
            MeshGeometry m;
            if (const PointCloud* p = in.points()) {
                m.vertices.reserve(p->size());
                for (const auto& pos : p->positions) {
                    Vertex v;
                    v.position = pos;
                    v.normal   = glm::vec3(0.0f, 0.0f, 1.0f);
                    v.tangent  = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
                    m.vertices.push_back(v);
                }
            }
            // No indices: these are loose vertices. Nothing draws them, but bounds and counts work,
            // and Join Geometry will carry them along.
            ctx.set_output("geometry", make_geometry(GeometrySet::from_mesh(std::move(m))));
        }});
}

} // namespace procgraph
} // namespace toy
