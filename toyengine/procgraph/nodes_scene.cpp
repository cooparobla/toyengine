/**
 * @file nodes_scene.cpp
 * @brief Nodes that read the scene snapshot -- the half that makes this an authoring tool.
 *
 * Every one of these reads an `ObjectRef` or `ObjectSet` that a scene-input resolver captured on
 * the main thread (see scene_input.h). None of them can reach the live scene, by construction:
 * the snapshot types carry no pointers.
 */

#include <toyengine/procgraph/nodes.h>

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>

namespace toy {
namespace procgraph {

void register_scene_nodes() {
    using coopa::nodegraph::register_node_type;

    register_node_type(NodeType{
        "ObjectInfo", "Scene", "Object Info",
        "Breaks a bound scene object into its transform and bounds", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("object", types().object_t);
            b.output("position", types().vector_t);
            b.output("rotation", types().rotation_t);
            b.output("scale", types().vector_t);
            b.output("bounds_min", types().vector_t);
            b.output("bounds_max", types().vector_t);
            b.output("size", types().vector_t);
            b.output("center", types().vector_t);
            b.output("geometry", types().geometry_t);
            b.output("exists", types().bool_t);
        },
        [](EvalContext& ctx) {
            const ObjectRef obj = detail::object_in(ctx, "object");
            ctx.set_output("position", make_vector(obj.position));
            ctx.set_output("rotation", make_rotation(obj.rotation));
            ctx.set_output("scale", make_vector(obj.scale));
            ctx.set_output("bounds_min", make_vector(obj.bounds_min));
            ctx.set_output("bounds_max", make_vector(obj.bounds_max));
            ctx.set_output("size", make_vector(obj.bounds_max - obj.bounds_min));
            ctx.set_output("center", make_vector((obj.bounds_max + obj.bounds_min) * 0.5f));
            ctx.set_output("geometry", make_geometry(obj.geometry));
            // `exists` rather than erroring on a missing object: a graph that places something
            // only when a marker is present is ordinary, and a missing marker is not a fault.
            ctx.set_output("exists", make_bool(obj.valid));
        }});

    register_node_type(NodeType{
        "SelfTransform", "Scene", "Self Transform",
        "The transform of the object this graph is attached to", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.output("object", types().object_t);
            b.output("position", types().vector_t);
            b.output("scale", types().vector_t);
        },
        [](EvalContext& ctx) {
            const BakeContext& bake = bake_context(ctx);
            ctx.set_output("object", make_object(bake.self));
            ctx.set_output("position", make_vector(bake.self.position));
            ctx.set_output("scale", make_vector(bake.self.scale));
        }});

    register_node_type(NodeType{
        "ObjectSetToPoints", "Scene", "Object Set to Points",
        "Turns a set of bound objects into a point cloud of their transforms -- how you "
        "hand-author placement and let the graph build",
        NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("objects", types().object_set_t);
            b.input_default("use_scale", types().bool_t, true);
            b.output("points", types().geometry_t);
            b.output("count", types().int_t);
        },
        [](EvalContext& ctx) {
            const ObjectSet set = detail::object_set_in(ctx, "objects");
            const bool use_scale = ctx.get<bool>("use_scale", true);
            PointCloud cloud;
            cloud.positions.reserve(set.size());
            cloud.rotations.reserve(set.size());
            if (use_scale) cloud.scales.reserve(set.size());
            for (size_t i = 0; i < set.size(); ++i) {
                const ObjectRef& ref = set.at(i);
                if (!ref.valid) continue;
                cloud.positions.push_back(ref.position);
                cloud.rotations.push_back(ref.rotation);
                if (use_scale) cloud.scales.push_back(ref.scale);
            }
            const auto count = static_cast<int32_t>(cloud.positions.size());
            ctx.set_output("points", make_geometry(GeometrySet::from_points(std::move(cloud))));
            ctx.set_output("count", make_int(count));
        }});

    register_node_type(NodeType{
        "ObjectSetFilter", "Scene", "Object Set Filter",
        "Keeps the bound objects within a radius of a point", NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("objects", types().object_set_t);
            b.input_default("center", types().vector_t, glm::vec3(0.0f));
            b.input_default("radius", types().float_t, 10.0f);
            b.input_default("invert", types().bool_t, false);
            b.output("objects", types().object_set_t);
            b.output("count", types().int_t);
        },
        [](EvalContext& ctx) {
            const ObjectSet set = detail::object_set_in(ctx, "objects");
            const glm::vec3 center = ctx.get<glm::vec3>("center", glm::vec3(0.0f));
            const float radius = ctx.get<float>("radius", 10.0f);
            const bool invert = ctx.get<bool>("invert", false);
            const float r2 = radius * radius;

            std::vector<ObjectRef> kept;
            kept.reserve(set.size());
            for (size_t i = 0; i < set.size(); ++i) {
                const ObjectRef& ref = set.at(i);
                if (!ref.valid) continue;
                const glm::vec3 d = ref.position - center;
                const bool inside = glm::dot(d, d) <= r2;
                if (inside != invert) kept.push_back(ref);
            }
            const auto count = static_cast<int32_t>(kept.size());
            ctx.set_output("objects", make_object_set(ObjectSet::from(std::move(kept))));
            ctx.set_output("count", make_int(count));
        }});

    register_node_type(NodeType{
        "ObjectSetBounds", "Scene", "Object Set Bounds",
        "The combined world bounds of a set of bound objects", NodeCost::Cheap,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("objects", types().object_set_t);
            b.output("min", types().vector_t);
            b.output("max", types().vector_t);
            b.output("center", types().vector_t);
            b.output("size", types().vector_t);
        },
        [](EvalContext& ctx) {
            const ObjectSet set = detail::object_set_in(ctx, "objects");
            glm::vec3 lo(0.0f);
            glm::vec3 hi(0.0f);
            bool first = true;
            for (size_t i = 0; i < set.size(); ++i) {
                const ObjectRef& ref = set.at(i);
                if (!ref.valid) continue;
                // An object with no renderer reports zero bounds, so its POSITION is used instead
                // of a degenerate box at the origin, which would drag the union to the origin.
                const bool has_box = ref.bounds_min != ref.bounds_max;
                const glm::vec3 a = has_box ? ref.bounds_min : ref.position;
                const glm::vec3 b = has_box ? ref.bounds_max : ref.position;
                if (first) {
                    lo = a;
                    hi = b;
                    first = false;
                } else {
                    lo = glm::min(lo, a);
                    hi = glm::max(hi, b);
                }
            }
            ctx.set_output("min", make_vector(lo));
            ctx.set_output("max", make_vector(hi));
            ctx.set_output("center", make_vector((lo + hi) * 0.5f));
            ctx.set_output("size", make_vector(hi - lo));
        }});
}

} // namespace procgraph
} // namespace toy
