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

/**
 * @brief Appends a tube to `mesh`: one ring of `sides` vertices per spine point, quads between.
 *
 * The rings are SHARED between consecutive sections rather than each section being its own
 * capped tube. That is what makes the result one continuous surface: there is no joint to open
 * on the outside of a bend, and no interior face to show through one.
 *
 * Normals are radial from the spine, so the tube shades smoothly around its circumference
 * however many sides it has. Open-ended: a tube is either buried in its parent at one end and
 * tapering to nothing at the other, or part of a longer run.
 *
 * @param frames Per-spine-point orientation; its +Y/+Z span the ring plane. Parallel-transported
 *               by the caller, since a per-point frame would twist the quads against each other.
 */
void sweep_rings(MeshGeometry& mesh, const std::vector<glm::vec3>& spine,
                 const std::vector<glm::quat>& frames, int sides, float r0, float r1) {
    const std::size_t n = std::min(spine.size(), frames.size());
    if (n < 2 || sides < 3) return;

    const auto first = static_cast<uint32_t>(mesh.vertices.size());
    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(n - 1);
        const float r = glm::mix(r0, r1, t);
        const glm::mat3 basis = glm::mat3_cast(frames[i]);
        for (int k = 0; k < sides; ++k) {
            const float a = 2.0f * glm::pi<float>() * static_cast<float>(k) / static_cast<float>(sides);
            const glm::vec3 radial = basis[1] * std::cos(a) + basis[2] * std::sin(a);
            Vertex v;
            v.position = spine[i] + radial * r;
            v.normal   = radial;
            v.uv       = glm::vec2(static_cast<float>(k) / static_cast<float>(sides), t);
            v.tangent  = glm::vec4(basis[0], 1.0f);
            mesh.vertices.push_back(v);
        }
    }
    for (std::size_t i = 0; i + 1 < n; ++i) {
        for (int k = 0; k < sides; ++k) {
            const auto ring = static_cast<uint32_t>(i) * static_cast<uint32_t>(sides);
            const auto next = ring + static_cast<uint32_t>(sides);
            const auto a = first + ring + static_cast<uint32_t>(k);
            const auto b = first + ring + static_cast<uint32_t>((k + 1) % sides);
            const auto c = first + next + static_cast<uint32_t>((k + 1) % sides);
            const auto d = first + next + static_cast<uint32_t>(k);
            mesh.indices.insert(mesh.indices.end(), {a, b, c, a, c, d});
        }
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
            b.output("tips", types().geometry_t);
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
            // Where each instance ENDS, with the rotation it ended pointing. A prototype is
            // authored along +X (the direction a curve frame grows), so its far end is one unit
            // of local X away, scaled. Only this node can produce these: the spin it applies is
            // chosen in here, so a graph computing tips by hand would miss it and every child
            // level would hang off thin air instead of off its parent.
            PointCloud tips;
            tips.positions.reserve(group.transforms.size());
            tips.rotations.reserve(group.transforms.size());
            for (const glm::mat4& m : group.transforms) {
                tips.positions.push_back(glm::vec3(m[3]) + glm::vec3(m[0]));
                tips.rotations.push_back(glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(m[0])),
                                                                  glm::normalize(glm::vec3(m[1])),
                                                                  glm::normalize(glm::vec3(m[2])))));
            }

            const auto count = static_cast<int32_t>(group.transforms.size());
            ctx.set_output("tips", make_geometry(GeometrySet::from_points(std::move(tips))));
            ctx.set_output("instances", make_geometry(GeometrySet::from_instances(std::move(group))));
            ctx.set_output("count", make_int(count));
        }});

    // --- Swept tubes ---------------------------------------------------------------------------
    //
    // These build MESH geometry, not instances, and that is the point. A limb made of chained
    // instances is N separate tubes that merely abut: every joint is a seam, bends open a notch
    // on the outside, and the result is not one surface. A sweep shares the ring between
    // consecutive sections, so the tube is continuous by construction and no joint exists to
    // come apart.

    register_node_type(NodeType{
        "SweepCurve", "Curve", "Sweep Curve",
        "Sweeps a tapering tube ALONG a curve as one continuous mesh -- a trunk or a bough, with "
        "no joints to open at the bends",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("curve", types().curve_t);
            b.input_default("sides", types().int_t, int32_t(10));
            b.input_default("sections", types().int_t, int32_t(24));
            b.input_default("radius_start", types().float_t, 0.3f);
            b.input_default("radius_end", types().float_t, 0.08f);
            b.output("geometry", types().geometry_t);
            b.output("tip", types().geometry_t);
        },
        [](EvalContext& ctx) {
            const toy::curve::CurveData c = ctx.get<toy::curve::CurveData>("curve", empty_curve());
            const int sides = std::clamp(ctx.get<int32_t>("sides", 10), 3, 64);
            const int sections = std::clamp(ctx.get<int32_t>("sections", 24), 2, 512);
            const float r0 = std::max(0.0f, ctx.get<float>("radius_start", 0.3f));
            const float r1 = std::max(0.0f, ctx.get<float>("radius_end", 0.08f));

            std::vector<glm::vec3> tangents;
            const std::vector<glm::vec3> spine =
                toy::curve::points_along(c, sections, &tangents);
            if (spine.size() < 2) {
                ctx.set_output("geometry", make_geometry(GeometrySet()));
                ctx.set_output("tip", make_geometry(GeometrySet()));
                return;
            }
            // Parallel transport, so consecutive rings do not twist against each other and the
            // quads between them stay unsheared.
            const std::vector<glm::quat> frames = toy::curve::parallel_frames(tangents, 0.0f);

            MeshGeometry mesh;
            sweep_rings(mesh, spine, frames, sides, r0, r1);

            PointCloud tip;
            tip.positions.push_back(spine.back());
            tip.rotations.push_back(frames.back());
            ctx.set_output("geometry", make_geometry(GeometrySet::from_mesh(std::move(mesh))));
            ctx.set_output("tip", make_geometry(GeometrySet::from_points(std::move(tip))));
        }});

    register_node_type(NodeType{
        "TubesOnPoints", "Points", "Tubes on Points",
        "One tapering tube per point, grown along the point's own direction and merged into a "
        "single mesh. `sink` pushes each tube's base BACK into whatever it grows from, so a twig "
        "starts inside its branch instead of floating at its tip",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("points", types().geometry_t);
            b.input_default("length", types().float_t, 1.0f);
            b.input_default("sides", types().int_t, int32_t(8));
            b.input_default("sections", types().int_t, int32_t(5));
            b.input_default("radius_start", types().float_t, 0.08f);
            b.input_default("radius_end", types().float_t, 0.01f);
            b.input_default("sink", types().float_t, 0.02f);
            b.input_default("droop", types().float_t, 0.0f);
            b.input_default("spread", types().float_t, 0.0f);
            b.input_default("length_jitter", types().float_t, 0.0f);
            b.input_default("surface_points", types().int_t, int32_t(0));
            b.output("geometry", types().geometry_t);
            b.output("tips", types().geometry_t);
            b.output("surface", types().geometry_t);
            b.output("count", types().int_t);
        },
        [](EvalContext& ctx) {
            const GeometrySet in = detail::geometry_in(ctx, "points");
            const PointCloud* src = in.points();
            if (src == nullptr || src->empty()) {
                ctx.set_output("geometry", make_geometry(GeometrySet()));
                ctx.set_output("tips", make_geometry(GeometrySet()));
                ctx.set_output("count", make_int(0));
                return;
            }
            const float base_len = std::max(1e-4f, ctx.get<float>("length", 1.0f));
            const int sides = std::clamp(ctx.get<int32_t>("sides", 8), 3, 32);
            const int sections = std::clamp(ctx.get<int32_t>("sections", 5), 2, 64);
            const float r0 = std::max(0.0f, ctx.get<float>("radius_start", 0.08f));
            const float r1 = std::max(0.0f, ctx.get<float>("radius_end", 0.01f));
            // ABSOLUTE metres, not a fraction of `length`. What the base must stay inside is the
            // parent's RADIUS, which has nothing to do with how long the child is -- scaling it
            // by the child's length drove short twigs straight through thin branches.
            const float sink = std::max(0.0f, ctx.get<float>("sink", 0.02f));
            const float droop = ctx.get<float>("droop", 0.0f);
            const float spread = std::max(0.0f, ctx.get<float>("spread", 0.0f));
            const float jitter = std::max(0.0f, ctx.get<float>("length_jitter", 0.0f));
            const int per_tube = std::max(0, ctx.get<int32_t>("surface_points", 0));
            const uint32_t seed = bake_context(ctx).seed;

            MeshGeometry mesh;
            PointCloud tips;
            PointCloud surface;
            tips.positions.reserve(src->size());
            tips.rotations.reserve(src->size());

            for (std::size_t i = 0; i < src->size(); ++i) {
                glm::quat rot = src->rotation_of(i);
                const glm::vec3 scale = src->scale_of(i);
                // Fan each tube off its parent's direction, about the parent's own axes. Several
                // tubes sharing one origin then leave it in different directions -- which is
                // what twigs do at a branch end. Displacing their ORIGINS instead would scatter
                // them through the air around the tip, attached to nothing.
                if (spread > 0.0f) {
                    const auto key = static_cast<uint32_t>(i);
                    const float yaw = spread * (detail::hash01(seed ^ 0x7feb352du, key) * 2.0f - 1.0f);
                    const float pitch = spread * (detail::hash01(seed ^ 0x846ca68bu, key) * 2.0f - 1.0f);
                    rot = rot * glm::angleAxis(yaw, glm::vec3(0.0f, 0.0f, 1.0f)) *
                                glm::angleAxis(pitch, glm::vec3(0.0f, 1.0f, 0.0f));
                }
                const float wobble = jitter > 0.0f
                    ? 1.0f + jitter * (detail::hash01(seed ^ 0x1b873593u,
                                                      static_cast<uint32_t>(i)) * 2.0f - 1.0f)
                    : 1.0f;
                const float len = base_len * std::max(0.05f, scale.x) * std::max(0.05f, wobble);
                const float thick = std::max(0.02f, 0.5f * (scale.y + scale.z));

                // The spine runs from BEHIND the point (inside the parent) to the tip, bending
                // by `droop` so a twig is not a straight spike.
                std::vector<glm::vec3> spine;
                std::vector<glm::vec3> tangents;
                spine.reserve(static_cast<std::size_t>(sections));
                tangents.reserve(static_cast<std::size_t>(sections));
                const float start = -sink;
                for (int k = 0; k < sections; ++k) {
                    const float t = static_cast<float>(k) / static_cast<float>(sections - 1);
                    const float along = glm::mix(start, len, t);
                    const float bend = droop * len * t * t;
                    const glm::vec3 local(along, 0.0f, -bend);
                    spine.push_back(src->positions[i] + rot * local);
                }
                for (int k = 0; k < sections; ++k) {
                    const std::size_t a = k == 0 ? 0 : static_cast<std::size_t>(k) - 1;
                    const std::size_t b = k == 0 ? 1 : static_cast<std::size_t>(k);
                    glm::vec3 d = spine[b] - spine[a];
                    const float dl = glm::length(d);
                    tangents.push_back(dl > 1e-8f ? d / dl : rot * glm::vec3(1.0f, 0.0f, 0.0f));
                }
                const std::vector<glm::quat> frames = toy::curve::parallel_frames(tangents, 0.0f);
                sweep_rings(mesh, spine, frames, sides, r0 * thick, r1 * thick);

                tips.positions.push_back(spine.back());
                tips.rotations.push_back(frames.back());

                // Attachment points spread evenly ALONG this tube, on its surface.
                //
                // Not area-weighted sampling of the triangles: a tapered tube carries most of
                // its area at the fat end, so an area-weighted scatter crowds the base and
                // leaves the tip bare -- which on a tree reads as branches with nothing growing
                // near their ends. Walking the spine spends points by LENGTH instead, so the
                // thin outer half gets as many as the thick inner half.
                for (int k = 0; k < per_tube; ++k) {
                    // Offset by half a step: neither on the base ring nor exactly on the tip,
                    // both of which are already occupied by the parent joint and the tip child.
                    const float t = (static_cast<float>(k) + 0.5f) / static_cast<float>(per_tube);
                    const float fs = t * static_cast<float>(sections - 1);
                    const auto lo = static_cast<std::size_t>(fs);
                    const std::size_t hi = std::min(lo + 1, spine.size() - 1);
                    const float f = fs - static_cast<float>(lo);
                    const glm::vec3 at = glm::mix(spine[lo], spine[hi], f);
                    const glm::quat frame = glm::slerp(frames[lo], frames[hi], f);
                    const glm::mat3 basis = glm::mat3_cast(frame);

                    // Golden angle around the tube AND along it, so attachments spiral instead
                    // of lining up in a row down one side.
                    const float az = glm::radians(137.5f) *
                                     static_cast<float>(i * static_cast<std::size_t>(per_tube) + k);
                    const glm::vec3 radial = basis[1] * std::cos(az) + basis[2] * std::sin(az);
                    const float r = glm::mix(r0 * thick, r1 * thick, t);
                    surface.positions.push_back(at + radial * r);
                    surface.rotations.push_back(toy::curve::frame_from_tangent(radial, 0.0f));
                }
            }

            const auto count = static_cast<int32_t>(tips.positions.size());
            ctx.set_output("geometry", make_geometry(GeometrySet::from_mesh(std::move(mesh))));
            ctx.set_output("tips", make_geometry(GeometrySet::from_points(std::move(tips))));
            ctx.set_output("surface", make_geometry(GeometrySet::from_points(std::move(surface))));
            ctx.set_output("count", make_int(count));
        }});

    register_node_type(NodeType{
        "PointsOnFaces", "Points", "Points on Faces",
        "Scatters points across a mesh's actual TRIANGLES, area-weighted, each facing along the "
        "surface normal where it landed. What puts a twig's base ON the branch it grows from "
        "rather than somewhere near it",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("geometry", types().geometry_t);
            b.input_default("count", types().int_t, int32_t(32));
            b.input_default("align_to_normal", types().bool_t, true);
            b.output("points", types().geometry_t);
            b.output("count", types().int_t);
        },
        [](EvalContext& ctx) {
            const GeometrySet in = detail::geometry_in(ctx, "geometry");
            const MeshGeometry* mesh = in.mesh();
            const int want = std::max(0, ctx.get<int32_t>("count", 32));
            const bool align = ctx.get<bool>("align_to_normal", true);
            if (mesh == nullptr || want == 0 || mesh->triangle_count() == 0) {
                ctx.set_output("points", make_geometry(GeometrySet()));
                ctx.set_output("count", make_int(0));
                return;
            }

            // Area-weighted, so a long branch gets its share and a stubby one does not get the
            // same number of twigs as a trunk. A uniform pick per triangle would crowd wherever
            // the mesh happens to be densely tessellated.
            const std::size_t tris = mesh->triangle_count();
            std::vector<float> cumulative;
            cumulative.reserve(tris);
            float total = 0.0f;
            for (std::size_t t = 0; t < tris; ++t) {
                const glm::vec3& a = mesh->vertices[mesh->indices[t * 3 + 0]].position;
                const glm::vec3& b2 = mesh->vertices[mesh->indices[t * 3 + 1]].position;
                const glm::vec3& c = mesh->vertices[mesh->indices[t * 3 + 2]].position;
                total += 0.5f * glm::length(glm::cross(b2 - a, c - a));
                cumulative.push_back(total);
            }
            if (total <= 1e-9f) {
                ctx.set_output("points", make_geometry(GeometrySet()));
                ctx.set_output("count", make_int(0));
                return;
            }

            const uint32_t seed = bake_context(ctx).seed;
            PointCloud out;
            out.positions.reserve(static_cast<std::size_t>(want));
            if (align) out.rotations.reserve(static_cast<std::size_t>(want));

            for (int i = 0; i < want; ++i) {
                const auto key = static_cast<uint32_t>(i);
                const float pick = detail::hash01(seed ^ 0x3b9aca07u, key) * total;
                const std::size_t t = static_cast<std::size_t>(
                    std::lower_bound(cumulative.begin(), cumulative.end(), pick) - cumulative.begin());
                const std::size_t tri = std::min(t, tris - 1);

                // Uniform in the triangle: the sqrt folds the unit square onto it without
                // bunching along the hypotenuse, which a raw (u, v) pair would.
                float u = detail::hash01(seed ^ 0x9e3779b9u, key);
                float v = detail::hash01(seed ^ 0x85ebca6bu, key);
                const float su = std::sqrt(u);
                const float w0 = 1.0f - su;
                const float w1 = su * (1.0f - v);
                const float w2 = su * v;

                const Vertex& A = mesh->vertices[mesh->indices[tri * 3 + 0]];
                const Vertex& B = mesh->vertices[mesh->indices[tri * 3 + 1]];
                const Vertex& C = mesh->vertices[mesh->indices[tri * 3 + 2]];
                out.positions.push_back(A.position * w0 + B.position * w1 + C.position * w2);
                if (align) {
                    glm::vec3 n = A.normal * w0 + B.normal * w1 + C.normal * w2;
                    const float len = glm::length(n);
                    n = len > 1e-8f ? n / len
                                    : glm::normalize(glm::cross(B.position - A.position,
                                                                C.position - A.position));
                    // +X along the NORMAL: a tube grown on this point leaves the surface the way
                    // a twig leaves bark, rather than running along it.
                    out.rotations.push_back(toy::curve::frame_from_tangent(n, 0.0f));
                }
            }
            const auto count = static_cast<int32_t>(out.positions.size());
            ctx.set_output("points", make_geometry(GeometrySet::from_points(std::move(out))));
            ctx.set_output("count", make_int(count));
        }});

    register_node_type(NodeType{
        "ScatterPoints", "Points", "Scatter Points",
        "Spreads each point into `copies` of itself, jittered inside an ellipsoid of `radius`. "
        "What turns a row of points along a curve into a VOLUME -- a tree's crown rather than a "
        "column of leaves on its trunk",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("points", types().geometry_t);
            b.input_default("radius", types().vector_t, glm::vec3(1.0f));
            b.input_default("copies", types().int_t, int32_t(1));
            b.input_default("keep_frames", types().bool_t, true);
            b.input_default("scale_jitter", types().float_t, 0.0f);
            b.output("points", types().geometry_t);
            b.output("count", types().int_t);
        },
        [](EvalContext& ctx) {
            const GeometrySet in = detail::geometry_in(ctx, "points");
            const PointCloud* src = in.points();
            if (src == nullptr || src->empty()) {
                ctx.set_output("points", make_geometry(GeometrySet()));
                ctx.set_output("count", make_int(0));
                return;
            }
            const glm::vec3 radius = ctx.get<glm::vec3>("radius", glm::vec3(1.0f));
            const int copies = std::max(1, ctx.get<int32_t>("copies", 1));
            const bool keep = ctx.get<bool>("keep_frames", true);
            const float jitter = std::max(0.0f, ctx.get<float>("scale_jitter", 0.0f));
            const uint32_t seed = bake_context(ctx).seed;

            PointCloud out;
            const std::size_t total = src->size() * static_cast<std::size_t>(copies);
            out.positions.reserve(total);
            if (keep) out.rotations.reserve(total);
            if (keep || jitter > 0.0f) out.scales.reserve(total);

            for (std::size_t i = 0; i < src->size(); ++i) {
                for (int c = 0; c < copies; ++c) {
                    // Keyed on (bake seed, point, copy) and nothing else, so the same graph and
                    // seed scatter identically every run and on every thread -- the determinism
                    // contract the rest of the node library keeps.
                    const auto key = static_cast<uint32_t>(i * 977u + static_cast<uint32_t>(c));
                    // Rejection-free: three independent axes, each -1..1, then shaped to the
                    // ellipsoid by a cube-root radius so copies do not bunch at the centre.
                    glm::vec3 dir(detail::hash01(seed ^ 0x51ed270bu, key) * 2.0f - 1.0f,
                                  detail::hash01(seed ^ 0x2545f491u, key) * 2.0f - 1.0f,
                                  detail::hash01(seed ^ 0x9e3779b1u, key) * 2.0f - 1.0f);
                    const float len = glm::length(dir);
                    if (len > 1e-6f) {
                        const float r = std::cbrt(detail::hash01(seed ^ 0xa136aaadu, key));
                        dir = (dir / len) * r;
                    }
                    out.positions.push_back(src->positions[i] + dir * radius);
                    if (keep) out.rotations.push_back(src->rotation_of(i));
                    if (keep || jitter > 0.0f) {
                        const float j = jitter > 0.0f
                            ? 1.0f + jitter * (detail::hash01(seed ^ 0x27d4eb2fu, key) * 2.0f - 1.0f)
                            : 1.0f;
                        out.scales.push_back(src->scale_of(i) * std::max(0.01f, j));
                    }
                }
            }
            const auto count = static_cast<int32_t>(out.positions.size());
            ctx.set_output("points", make_geometry(GeometrySet::from_points(std::move(out))));
            ctx.set_output("count", make_int(count));
        }});

    register_node_type(NodeType{
        "CurveToPoints", "Curve", "Curve to Points",
        "Spreads `count` points evenly along a curve BY ARC LENGTH, each facing the way the "
        "curve runs -- what a railing, a fence or a row of lamps follows",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("curve", types().curve_t);
            b.input_default("count", types().int_t, int32_t(8));
            b.input_default("align_to_curve", types().bool_t, true);
            b.input_default("trim_start", types().float_t, 0.0f);
            b.input_default("trim_end", types().float_t, 1.0f);
            b.input_default("scale_start", types().float_t, 1.0f);
            b.input_default("scale_end", types().float_t, 1.0f);
            b.input_default("length_start", types().float_t, 1.0f);
            b.input_default("length_end", types().float_t, 1.0f);
            b.input_default("lean_start", types().float_t, 0.0f);
            b.input_default("lean_end", types().float_t, 0.0f);
            b.output("points", types().geometry_t);
            b.output("count", types().int_t);
            b.output("length", types().float_t);
            b.output("spacing", types().float_t);
        },
        [](EvalContext& ctx) {
            const toy::curve::CurveData c = ctx.get<toy::curve::CurveData>("curve", empty_curve());
            const int want = std::max(0, ctx.get<int32_t>("count", 8));
            const bool align = ctx.get<bool>("align_to_curve", true);
            const float trim_start = ctx.get<float>("trim_start", 0.0f);
            const float trim_end = ctx.get<float>("trim_end", 1.0f);
            const float scale_start = ctx.get<float>("scale_start", 1.0f);
            const float scale_end = ctx.get<float>("scale_end", 1.0f);
            const float length_start = ctx.get<float>("length_start", 1.0f);
            const float length_end = ctx.get<float>("length_end", 1.0f);
            const float lean_start = ctx.get<float>("lean_start", 0.0f);
            const float lean_end = ctx.get<float>("lean_end", 0.0f);

            std::vector<glm::vec3> tangents;
            PointCloud points;
            points.positions = toy::curve::points_along(c, want, align ? &tangents : nullptr,
                                                        trim_start, trim_end);
            if (align && tangents.size() == points.positions.size()) {
                // Parallel transport, not a frame per tangent: deriving each frame independently
                // from world up makes the ROLL jump as the curve turns, and consecutive
                // instances come out rolled differently -- mismatched facets on a tube, and
                // segments that read light or dark for no reason the author can see.
                const float tilt = c.points.empty() ? 0.0f : c.points.front().tilt;
                points.rotations = toy::curve::parallel_frames(tangents, tilt);

                // A LEAN profile turns a run of points into a branch profile: swing each frame
                // away from the curve by an angle that varies along it, and spiral the swings
                // around the curve so branches do not all leave in one plane. 137.5 degrees is
                // the golden angle -- the spacing real phyllotaxis uses, and the reason a spiral
                // of branches never lines up with itself.
                const bool leaning = std::abs(lean_start) > 1e-4f || std::abs(lean_end) > 1e-4f;
                if (leaning) {
                    const std::size_t n = points.rotations.size();
                    for (std::size_t i = 0; i < n; ++i) {
                        const float u = n < 2 ? 0.0f : static_cast<float>(i) / static_cast<float>(n - 1);
                        const float lean = glm::radians(glm::mix(lean_start, lean_end, u));
                        const float azimuth = glm::radians(137.5f) * static_cast<float>(i);
                        points.rotations[i] = points.rotations[i] *
                                              glm::angleAxis(azimuth, glm::vec3(1.0f, 0.0f, 0.0f)) *
                                              glm::angleAxis(lean, glm::vec3(0.0f, 0.0f, 1.0f));
                    }
                }
            }
            // Taper, as a per-point scale the instancer already honours. It narrows the CROSS
            // SECTION (y, z) and leaves the along-curve axis alone: `frame_from_tangent` puts
            // local +X along the tangent, so scaling that too would shorten each segment and
            // open gaps in what should be a continuous limb.
            const bool thickness_profile =
                std::abs(scale_start - scale_end) > 1e-6f || std::abs(scale_start - 1.0f) > 1e-6f;
            const bool length_profile =
                std::abs(length_start - length_end) > 1e-6f || std::abs(length_start - 1.0f) > 1e-6f;
            if (thickness_profile || length_profile) {
                const std::size_t n = points.positions.size();
                points.scales.reserve(n);
                for (std::size_t i = 0; i < n; ++i) {
                    const float u = n < 2 ? 0.0f : static_cast<float>(i) / static_cast<float>(n - 1);
                    // X is ALONG the run, y/z across it: a trunk tapers its cross section while
                    // keeping its segments meeting end to end, and a branch profile shortens the
                    // branches themselves without thinning them at the same rate.
                    points.scales.push_back(glm::vec3(glm::mix(length_start, length_end, u),
                                                      glm::mix(scale_start, scale_end, u),
                                                      glm::mix(scale_start, scale_end, u)));
                }
            }

            const auto count = static_cast<int32_t>(points.positions.size());
            const float total = toy::curve::length(c);
            const float trimmed = total * std::max(0.0f, std::min(trim_end, 1.0f) - std::max(trim_start, 0.0f));
            ctx.set_output("points", make_geometry(GeometrySet::from_points(std::move(points))));
            ctx.set_output("count", make_int(count));
            ctx.set_output("length", make_float(total));
            // What one segment must span for consecutive instances to meet end to end, which is
            // what a trunk built of segments needs and is fiddly to derive in the graph.
            ctx.set_output("spacing", make_float(count > 1 ? trimmed / static_cast<float>(count - 1) : trimmed));
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
