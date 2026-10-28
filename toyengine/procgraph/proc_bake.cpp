/**
 * @file proc_bake.cpp
 * @brief Running the graph, and reading its outputs into an object tree.
 */

#include <toyengine/procgraph/proc_bake.h>

#include <memory>

#include <coopa/nodegraph/evaluator.h>
#include <toyengine/procgraph/nodes.h>
#include <toyengine/procgraph/proc_types.h>

namespace toy {
namespace procgraph {
namespace {

using coopa::nodegraph::EvalRequest;
using coopa::nodegraph::EvalResult;
using coopa::nodegraph::Value;

/// @brief Attaches a spec's geometry to a baked object, splitting mesh from instances.
void attach_geometry_(const GeometrySet& geo, BakedObject& out) {
    if (const MeshGeometry* m = geo.mesh(); m != nullptr && !m->empty()) {
        out.mesh = std::make_shared<const MeshGeometry>(*m);
    }
    if (geo.has_instances()) {
        out.instances = std::make_shared<const std::vector<InstanceGroup>>(geo.instances());
    }
    // A point cloud with neither mesh nor instances is deliberately dropped here: nothing in the
    // engine draws loose points, so carrying them to the publish step would only mislead. A graph
    // that wants points on screen runs them through Instance on Points first.
}

} // namespace

BakedObject object_from_spec(const ObjectSpec& spec, const std::string& fallback_name) {
    const ObjectSpec::Data& data = spec.get();
    BakedObject out;
    out.name     = data.name.empty() ? fallback_name : data.name;
    out.position = data.position;
    out.rotation = data.rotation;
    out.scale    = data.scale;

    attach_geometry_(data.geometry, out);

    out.components.reserve(data.components.size());
    for (const ComponentSpec& c : data.components) {
        if (c.type.empty()) continue;
        // A MeshCollider emitted by Make Collider carries the INTENT; the shape itself is built by
        // proc_publish from this object's own geometry, so the collider flag travels separately.
        if (c.type == "MeshCollider") {
            const bool convex = c.props.is_mapping() && c.props.contains("convex") &&
                                c.props.at("convex").is_boolean() &&
                                c.props.at("convex").get_value<bool>();
            out.collider = convex ? BakedObject::Collider::Convex : BakedObject::Collider::Trimesh;
        }
        out.components.push_back(BakedComponent(c.type, c.props));
    }
    if (data.collider == 1) out.collider = BakedObject::Collider::Trimesh;
    else if (data.collider == 2) out.collider = BakedObject::Collider::Convex;

    out.children.reserve(data.children.size());
    for (size_t i = 0; i < data.children.size(); ++i) {
        out.children.push_back(object_from_spec(data.children[i],
                                                out.name + "_" + std::to_string(i)));
    }
    return out;
}

BakeResult bake(const BakeRequest& request) {
    BakeResult result;
    result.input_hash = request.input_hash;
    result.root.name  = request.context.graph_name.empty() ? "Generated"
                                                           : request.context.graph_name;

    if (request.graph == nullptr) {
        result.errors.push_back("bake() called with no compiled graph");
        return result;
    }

    register_procgraph_nodes();

    EvalRequest eval;
    eval.graph     = request.graph;
    eval.inputs    = request.inputs;
    eval.jobs      = request.jobs;
    eval.cancel    = request.cancel;
    eval.priority  = coopa::job::Priority::Low;   // A bake must never preempt frame work.
    // The bake context travels as the library's one opaque pointer; const_cast because the
    // library's user_data is a void* by design (it cannot know the consumer's type), and nothing
    // downstream writes through it -- every node reads it via bake_context(), which returns const.
    eval.user_data = const_cast<BakeContext*>(&request.context);

    EvalResult evaluated = coopa::nodegraph::evaluate(eval);

    for (const auto& entry : evaluated.diagnostics.entries()) {
        std::string line = entry.message;
        if (!entry.node_label.empty()) {
            line = entry.node_label + " (#" + std::to_string(entry.node) + "): " + line;
        }
        if (entry.severity == coopa::nodegraph::Severity::Error) result.errors.push_back(line);
        else result.warnings.push_back(line);
    }

    // --- Read the outputs, in the order of preference the @file doc sets out ---

    const auto& sockets = request.graph->outputs();
    std::vector<const Value*> components;
    const Value* geometry = nullptr;
    const Value* object = nullptr;

    for (size_t i = 0; i < sockets.size() && i < evaluated.outputs.size(); ++i) {
        const Value& v = evaluated.outputs[i];
        if (v.empty()) continue;
        if (v.type() == types().object_spec_t && object == nullptr) object = &v;
        else if (v.type() == types().geometry_t && geometry == nullptr) geometry = &v;
        else if (v.type() == types().component_t) components.push_back(&v);
    }

    if (object != nullptr) {
        static const ObjectSpec none;
        result.root = object_from_spec(object->as<ObjectSpec>(none), result.root.name);
    } else if (geometry != nullptr) {
        attach_geometry_(geometry->as<GeometrySet>(empty_geometry()), result.root);
    }

    // Loose Component outputs are attached to the root whichever branch ran, so a graph can emit
    // an Object Output tree AND a component beside it.
    for (const Value* v : components) {
        static const ComponentSpec none;
        const ComponentSpec spec = v->as<ComponentSpec>(none);
        if (spec.type.empty()) continue;
        if (spec.type == "MeshCollider" && result.root.collider == BakedObject::Collider::None) {
            result.root.collider = BakedObject::Collider::Trimesh;
        }
        result.root.components.push_back(BakedComponent(spec.type, spec.props));
    }

    if (object == nullptr && geometry == nullptr && components.empty() && result.errors.empty()) {
        // Not an error: a graph under construction legitimately produces nothing yet. But it is
        // worth saying, because an unwired Group Output is the single most common reason a graph
        // appears to "do nothing".
        result.warnings.push_back(
            "the graph produced no geometry, object or component -- is its Group Output wired up?");
    }

    return result;
}

} // namespace procgraph
} // namespace toy
