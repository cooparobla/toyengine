/**
 * @file nodes_primitives.cpp
 * @brief Geometry generators: the shapes a graph starts from.
 *
 * Every one emits a `MeshGeometry` in its own local space, centred on the origin unless the shape
 * makes another choice obvious (a cylinder and a cone sit on z = 0, because that is where you
 * want a post or a tree trunk). Z is up, matching the rest of this engine.
 *
 * Normals are built explicitly rather than left to `recompute_normals()` wherever a shape has
 * hard edges: a cube averaged per shared vertex would come out soft and wrong. Where a shape is
 * genuinely smooth (a sphere, a cylinder's wall) the analytic normal is exact and cheaper.
 */

#include <toyengine/procgraph/nodes.h>

#include <algorithm>
#include <cmath>

namespace toy {
namespace procgraph {
namespace {

constexpr float k_pi = 3.14159265358979323846f;

/// @brief Appends one triangle, with per-corner normals and UVs already decided.
void push_tri(MeshGeometry& m, const Vertex& a, const Vertex& b, const Vertex& c) {
    const auto base = static_cast<uint32_t>(m.vertices.size());
    m.vertices.push_back(a);
    m.vertices.push_back(b);
    m.vertices.push_back(c);
    m.indices.push_back(base);
    m.indices.push_back(base + 1);
    m.indices.push_back(base + 2);
}

Vertex vert(const glm::vec3& p, const glm::vec3& n, const glm::vec2& uv) {
    Vertex v;
    v.position = p;
    v.normal   = n;
    v.uv       = uv;
    // A tangent perpendicular to the normal. Generated geometry has no authored UV gradient worth
    // deriving a real tangent basis from, and a zero tangent breaks normal mapping outright.
    const glm::vec3 up = std::abs(n.z) < 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                               : glm::vec3(1.0f, 0.0f, 0.0f);
    v.tangent = glm::vec4(glm::normalize(glm::cross(up, n)), 1.0f);
    return v;
}

/// @brief Appends a flat quad (a, b, c, d counter-clockwise seen from +normal) as two triangles.
void push_quad(MeshGeometry& m, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
               const glm::vec3& d, const glm::vec3& n) {
    push_tri(m, vert(a, n, {0.0f, 0.0f}), vert(b, n, {1.0f, 0.0f}), vert(c, n, {1.0f, 1.0f}));
    push_tri(m, vert(a, n, {0.0f, 0.0f}), vert(c, n, {1.0f, 1.0f}), vert(d, n, {0.0f, 1.0f}));
}

MeshGeometry make_cube(const glm::vec3& size) {
    const glm::vec3 h = size * 0.5f;
    MeshGeometry m;
    m.vertices.reserve(36);
    m.indices.reserve(36);
    // Six faces, each with its own four vertices: a cube shares positions between faces but not
    // normals, so sharing vertices would force a single averaged normal per corner.
    push_quad(m, {-h.x, -h.y,  h.z}, { h.x, -h.y,  h.z}, { h.x,  h.y,  h.z}, {-h.x,  h.y,  h.z}, { 0, 0, 1});
    push_quad(m, { h.x, -h.y, -h.z}, {-h.x, -h.y, -h.z}, {-h.x,  h.y, -h.z}, { h.x,  h.y, -h.z}, { 0, 0,-1});
    push_quad(m, {-h.x, -h.y, -h.z}, { h.x, -h.y, -h.z}, { h.x, -h.y,  h.z}, {-h.x, -h.y,  h.z}, { 0,-1, 0});
    push_quad(m, { h.x,  h.y, -h.z}, {-h.x,  h.y, -h.z}, {-h.x,  h.y,  h.z}, { h.x,  h.y,  h.z}, { 0, 1, 0});
    push_quad(m, { h.x, -h.y, -h.z}, { h.x,  h.y, -h.z}, { h.x,  h.y,  h.z}, { h.x, -h.y,  h.z}, { 1, 0, 0});
    push_quad(m, {-h.x,  h.y, -h.z}, {-h.x, -h.y, -h.z}, {-h.x, -h.y,  h.z}, {-h.x,  h.y,  h.z}, {-1, 0, 0});
    return m;
}

MeshGeometry make_grid(float size_x, float size_y, int nx, int ny) {
    nx = std::clamp(nx, 1, 512);
    ny = std::clamp(ny, 1, 512);
    MeshGeometry m;
    const int vx = nx + 1;
    const int vy = ny + 1;
    m.vertices.reserve(static_cast<size_t>(vx) * vy);
    for (int y = 0; y < vy; ++y) {
        for (int x = 0; x < vx; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(nx);
            const float v = static_cast<float>(y) / static_cast<float>(ny);
            m.vertices.push_back(vert({(u - 0.5f) * size_x, (v - 0.5f) * size_y, 0.0f},
                                      {0.0f, 0.0f, 1.0f}, {u, v}));
        }
    }
    m.indices.reserve(static_cast<size_t>(nx) * ny * 6);
    for (int y = 0; y < ny; ++y) {
        for (int x = 0; x < nx; ++x) {
            const auto i0 = static_cast<uint32_t>(y * vx + x);
            const auto i1 = i0 + 1;
            const auto i2 = i0 + static_cast<uint32_t>(vx);
            const auto i3 = i2 + 1;
            m.indices.insert(m.indices.end(), {i0, i1, i3, i0, i3, i2});
        }
    }
    return m;
}

MeshGeometry make_cylinder(float radius, float height, int segments, bool caps) {
    segments = std::clamp(segments, 3, 256);
    MeshGeometry m;
    for (int i = 0; i < segments; ++i) {
        const float a0 = 2.0f * k_pi * static_cast<float>(i) / static_cast<float>(segments);
        const float a1 = 2.0f * k_pi * static_cast<float>(i + 1) / static_cast<float>(segments);
        const glm::vec2 d0(std::cos(a0), std::sin(a0));
        const glm::vec2 d1(std::cos(a1), std::sin(a1));
        const glm::vec3 p00(d0.x * radius, d0.y * radius, 0.0f);
        const glm::vec3 p10(d1.x * radius, d1.y * radius, 0.0f);
        const glm::vec3 p01(d0.x * radius, d0.y * radius, height);
        const glm::vec3 p11(d1.x * radius, d1.y * radius, height);
        const glm::vec3 n0(d0.x, d0.y, 0.0f);
        const glm::vec3 n1(d1.x, d1.y, 0.0f);
        const float u0 = static_cast<float>(i) / static_cast<float>(segments);
        const float u1 = static_cast<float>(i + 1) / static_cast<float>(segments);
        // Per-corner side normals, so the wall shades as a smooth curve rather than as facets.
        push_tri(m, vert(p00, n0, {u0, 0.0f}), vert(p10, n1, {u1, 0.0f}), vert(p11, n1, {u1, 1.0f}));
        push_tri(m, vert(p00, n0, {u0, 0.0f}), vert(p11, n1, {u1, 1.0f}), vert(p01, n0, {u0, 1.0f}));

        if (caps) {
            const glm::vec3 up(0.0f, 0.0f, 1.0f);
            push_tri(m, vert({0.0f, 0.0f, height}, up, {0.5f, 0.5f}),
                        vert(p01, up, {d0.x * 0.5f + 0.5f, d0.y * 0.5f + 0.5f}),
                        vert(p11, up, {d1.x * 0.5f + 0.5f, d1.y * 0.5f + 0.5f}));
            push_tri(m, vert({0.0f, 0.0f, 0.0f}, -up, {0.5f, 0.5f}),
                        vert(p10, -up, {d1.x * 0.5f + 0.5f, d1.y * 0.5f + 0.5f}),
                        vert(p00, -up, {d0.x * 0.5f + 0.5f, d0.y * 0.5f + 0.5f}));
        }
    }
    return m;
}

MeshGeometry make_cone(float radius, float height, int segments, bool cap) {
    segments = std::clamp(segments, 3, 256);
    MeshGeometry m;
    const glm::vec3 apex(0.0f, 0.0f, height);
    for (int i = 0; i < segments; ++i) {
        const float a0 = 2.0f * k_pi * static_cast<float>(i) / static_cast<float>(segments);
        const float a1 = 2.0f * k_pi * static_cast<float>(i + 1) / static_cast<float>(segments);
        const glm::vec3 p0(std::cos(a0) * radius, std::sin(a0) * radius, 0.0f);
        const glm::vec3 p1(std::cos(a1) * radius, std::sin(a1) * radius, 0.0f);
        // The side normal leans by the cone's own slope, which a radial normal would not capture.
        const glm::vec3 side = glm::normalize(glm::cross(p1 - p0, apex - p0));
        push_tri(m, vert(p0, side, {0.0f, 0.0f}), vert(p1, side, {1.0f, 0.0f}),
                    vert(apex, side, {0.5f, 1.0f}));
        if (cap) {
            const glm::vec3 down(0.0f, 0.0f, -1.0f);
            push_tri(m, vert({0.0f, 0.0f, 0.0f}, down, {0.5f, 0.5f}),
                        vert(p1, down, {1.0f, 0.0f}), vert(p0, down, {0.0f, 0.0f}));
        }
    }
    return m;
}

MeshGeometry make_sphere(float radius, int segments, int rings) {
    segments = std::clamp(segments, 3, 256);
    rings    = std::clamp(rings, 2, 256);
    MeshGeometry m;
    // A UV sphere about the Z axis. Poles are degenerate quads, emitted as single triangles.
    for (int r = 0; r < rings; ++r) {
        const float v0 = static_cast<float>(r) / static_cast<float>(rings);
        const float v1 = static_cast<float>(r + 1) / static_cast<float>(rings);
        const float phi0 = v0 * k_pi;
        const float phi1 = v1 * k_pi;
        for (int s = 0; s < segments; ++s) {
            const float u0 = static_cast<float>(s) / static_cast<float>(segments);
            const float u1 = static_cast<float>(s + 1) / static_cast<float>(segments);
            const float th0 = u0 * 2.0f * k_pi;
            const float th1 = u1 * 2.0f * k_pi;
            auto on_sphere = [radius](float phi, float theta) {
                return glm::vec3(std::sin(phi) * std::cos(theta) * radius,
                                 std::sin(phi) * std::sin(theta) * radius,
                                 std::cos(phi) * radius);
            };
            const glm::vec3 a = on_sphere(phi0, th0);
            const glm::vec3 b = on_sphere(phi1, th0);
            const glm::vec3 c = on_sphere(phi1, th1);
            const glm::vec3 d = on_sphere(phi0, th1);
            auto nrm = [radius](const glm::vec3& p) {
                return radius > 1e-8f ? p / radius : glm::vec3(0.0f, 0.0f, 1.0f);
            };
            if (r != 0) {
                push_tri(m, vert(a, nrm(a), {u0, v0}), vert(b, nrm(b), {u0, v1}),
                            vert(c, nrm(c), {u1, v1}));
            }
            if (r != rings - 1) {
                push_tri(m, vert(a, nrm(a), {u0, v0}), vert(c, nrm(c), {u1, v1}),
                            vert(d, nrm(d), {u1, v0}));
            }
        }
    }
    return m;
}

/// @brief A ring of points as a point cloud, which is how a "circle" is useful to a graph.
PointCloud make_circle_points(float radius, int count) {
    count = std::clamp(count, 1, 100000);
    PointCloud cloud;
    cloud.positions.reserve(count);
    for (int i = 0; i < count; ++i) {
        const float a = 2.0f * k_pi * static_cast<float>(i) / static_cast<float>(count);
        cloud.positions.emplace_back(std::cos(a) * radius, std::sin(a) * radius, 0.0f);
    }
    return cloud;
}

PointCloud make_line_points(const glm::vec3& start, const glm::vec3& end, int count) {
    count = std::clamp(count, 1, 1000000);
    PointCloud cloud;
    cloud.positions.reserve(count);
    if (count == 1) {
        cloud.positions.push_back(start);
        return cloud;
    }
    for (int i = 0; i < count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count - 1);
        cloud.positions.push_back(glm::mix(start, end, t));
    }
    return cloud;
}

} // namespace

void register_primitive_nodes() {
    using coopa::nodegraph::register_node_type;

    register_node_type(NodeType{
        "Cube", "Primitives", "Cube", {}, NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("size", types().vector_t, glm::vec3(1.0f));
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            const glm::vec3 size = ctx.get<glm::vec3>("size", glm::vec3(1.0f));
            ctx.set_output("geometry", make_geometry(GeometrySet::from_mesh(make_cube(size))));
        }});

    register_node_type(NodeType{
        "Grid", "Primitives", "Grid", "A subdivided plane in XY", NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("size_x", types().float_t, 1.0f);
            b.input_default("size_y", types().float_t, 1.0f);
            b.input_default("verts_x", types().int_t, int32_t(2));
            b.input_default("verts_y", types().int_t, int32_t(2));
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            // Authored as vertex counts, as Blender's Grid node is; a count of N is N-1 quads.
            const int nx = std::max(1, ctx.get<int32_t>("verts_x", 2) - 1);
            const int ny = std::max(1, ctx.get<int32_t>("verts_y", 2) - 1);
            MeshGeometry m = make_grid(ctx.get<float>("size_x", 1.0f),
                                       ctx.get<float>("size_y", 1.0f), nx, ny);
            ctx.set_output("geometry", make_geometry(GeometrySet::from_mesh(std::move(m))));
        }});

    register_node_type(NodeType{
        "Cylinder", "Primitives", "Cylinder", "Sits on z = 0, so it reads as a post",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("radius", types().float_t, 0.5f);
            b.input_default("height", types().float_t, 1.0f);
            b.input_default("segments", types().int_t, int32_t(16));
            b.input_default("caps", types().bool_t, true);
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            MeshGeometry m = make_cylinder(ctx.get<float>("radius", 0.5f),
                                           ctx.get<float>("height", 1.0f),
                                           ctx.get<int32_t>("segments", 16),
                                           ctx.get<bool>("caps", true));
            ctx.set_output("geometry", make_geometry(GeometrySet::from_mesh(std::move(m))));
        }});

    register_node_type(NodeType{
        "Cone", "Primitives", "Cone", {}, NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("radius", types().float_t, 0.5f);
            b.input_default("height", types().float_t, 1.0f);
            b.input_default("segments", types().int_t, int32_t(16));
            b.input_default("cap", types().bool_t, true);
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            MeshGeometry m = make_cone(ctx.get<float>("radius", 0.5f),
                                       ctx.get<float>("height", 1.0f),
                                       ctx.get<int32_t>("segments", 16),
                                       ctx.get<bool>("cap", true));
            ctx.set_output("geometry", make_geometry(GeometrySet::from_mesh(std::move(m))));
        }});

    register_node_type(NodeType{
        "Sphere", "Primitives", "Sphere", "A UV sphere about Z", NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("radius", types().float_t, 0.5f);
            b.input_default("segments", types().int_t, int32_t(16));
            b.input_default("rings", types().int_t, int32_t(8));
            b.output("geometry", types().geometry_t);
        },
        [](EvalContext& ctx) {
            MeshGeometry m = make_sphere(ctx.get<float>("radius", 0.5f),
                                         ctx.get<int32_t>("segments", 16),
                                         ctx.get<int32_t>("rings", 8));
            ctx.set_output("geometry", make_geometry(GeometrySet::from_mesh(std::move(m))));
        }});

    register_node_type(NodeType{
        "LinePoints", "Primitives", "Line", "Evenly spaced points between two ends",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("start", types().vector_t, glm::vec3(0.0f));
            b.input_default("end", types().vector_t, glm::vec3(0.0f, 0.0f, 1.0f));
            b.input_default("count", types().int_t, int32_t(2));
            b.output("points", types().geometry_t);
        },
        [](EvalContext& ctx) {
            PointCloud c = make_line_points(ctx.get<glm::vec3>("start", glm::vec3(0.0f)),
                                            ctx.get<glm::vec3>("end", glm::vec3(0.0f, 0.0f, 1.0f)),
                                            ctx.get<int32_t>("count", 2));
            ctx.set_output("points", make_geometry(GeometrySet::from_points(std::move(c))));
        }});

    register_node_type(NodeType{
        "CirclePoints", "Primitives", "Circle", "Points evenly around a ring", NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("radius", types().float_t, 1.0f);
            b.input_default("count", types().int_t, int32_t(8));
            b.output("points", types().geometry_t);
        },
        [](EvalContext& ctx) {
            PointCloud c = make_circle_points(ctx.get<float>("radius", 1.0f),
                                              ctx.get<int32_t>("count", 8));
            ctx.set_output("points", make_geometry(GeometrySet::from_points(std::move(c))));
        }});
}

} // namespace procgraph
} // namespace toy
