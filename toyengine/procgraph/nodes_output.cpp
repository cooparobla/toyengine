/**
 * @file nodes_output.cpp
 * @brief The Output category: where a graph says what it produces.
 *
 * This is why the `ProceduralGraph` component has no `collider:`, `output:` or `emit:` key. What a
 * graph makes is decided by the nodes in it, because a graph may be doing something that has
 * nothing to do with meshes.
 *
 * `AddComponent` is the whole generality of the output side: a component is a registered type name
 * plus the keys a scene file would write, and `proc_publish` hands those to
 * `SceneLoader::build_object()`, which dispatches through the process-wide
 * `register_component_parser` table. So a graph can emit a `Light`, a `Rigidbody`, an
 * `AudioSource`, a `ParticleSystem` or a game project's own `TOY_MODULE()` component with no code
 * here knowing any of those exist. `MakeCollider` and `MakeInstancer` are convenience wrappers
 * over it, placed in the graph where an author expects to find them.
 */

#include <toyengine/procgraph/nodes.h>

#include <algorithm>

#include <toyengine/core/yaml_read.h>

namespace toy {
namespace procgraph {
namespace {

/// @brief Builds a props mapping from a node's own `props:` block, minus the keys we consume.
fkyaml::node props_passthrough(const PropertyBag& bag, const std::vector<std::string>& consumed) {
    fkyaml::node out = fkyaml::node::mapping();
    const fkyaml::node& raw = bag.raw();
    if (!raw.is_mapping()) return out;
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        const std::string key = it.key().get_value<std::string>();
        if (std::find(consumed.begin(), consumed.end(), key) != consumed.end()) continue;
        out[key] = it.value();
    }
    return out;
}

} // namespace

void register_output_nodes() {
    using coopa::nodegraph::register_node_type;

    register_node_type(NodeType{
        "AddComponent", "Output", "Add Component",
        "Emits any registered component -- a Light, a Rigidbody, a game's own type",
        NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("type", types().string_t, std::string());
            b.output("component", types().component_t);
        },
        [](EvalContext& ctx) {
            // The type may come from a socket or from the node's own `type` prop, so a graph can
            // either hard-code it or compute it.
            std::string type = ctx.get<std::string>("type", std::string());
            if (type.empty()) type = ctx.props().str("type");
            if (type.empty()) {
                ctx.error("Add Component needs a component type name");
                return;
            }
            // Everything in `props:` except `type` becomes the component's keys verbatim, so a
            // component type this build has never heard of still authors correctly.
            fkyaml::node props = props_passthrough(ctx.props(), {"type"});
            ctx.set_output("component", make_component(ComponentSpec(std::move(type), std::move(props))));
        }});

    register_node_type(NodeType{
        "MakeCollider", "Output", "Make Collider",
        "Marks geometry as collidable, emitting a MeshCollider component", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("convex", types().bool_t, false);
            b.input_default("physics_material", types().string_t, std::string());
            b.output("component", types().component_t);
        },
        [](EvalContext& ctx) {
            // The SHAPE is not carried here: a trimesh of a baked mesh is far too large for YAML,
            // so proc_publish builds it from the object's own geometry and attaches it through
            // MeshCollider::set_mesh(shared_ptr). This component only records the intent and the
            // settings that *are* authorable.
            fkyaml::node props = props_passthrough(ctx.props(), {"convex", "physics_material"});
            if (ctx.get<bool>("convex", false)) props["convex"] = fkyaml::node(true);
            const std::string material = ctx.get<std::string>("physics_material", std::string());
            if (!material.empty()) props["physics_material"] = fkyaml::node(material);
            ctx.set_output("component",
                           make_component(ComponentSpec("MeshCollider", std::move(props))));
        }});

    register_node_type(NodeType{
        "MakeInstancer", "Output", "Make Instancer",
        "Turns instance groups into an InstancePainter, so the existing instance system draws "
        "them, culls them, thins them by distance and streams their collision",
        NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input("instances", types().geometry_t);
            b.input_default("collision", types().bool_t, false);
            b.input_default("draw_distance", types().float_t, 0.0f);
            b.output("component", types().component_t);
        },   // `kind` is a node property, not a socket: it selects which key the component
             // carries, which is a shape decision rather than a value a graph would compute.
        [](EvalContext& ctx) {
            const GeometrySet geo = detail::geometry_in(ctx, "instances");
            if (!geo.has_instances()) {
                ctx.warn("Make Instancer was given no instance groups");
                return;
            }
            // The prototypes list is what an InstancePainter needs; the transforms themselves are
            // too large for YAML and ride on the BakedObject instead (see proc_publish).
            // An InstancePrototype takes EITHER `object:` (an object asset, which may carry
            // several renderers and a collider) or `mesh:` (a bare mesh). They are not
            // interchangeable: handing a mesh path to `object:` fails to instantiate, with a
            // warning from the painter and nothing on screen. So the graph says which it means.
            const bool as_mesh = ctx.props().str("kind", "object") == "mesh";
            fkyaml::node props = fkyaml::node::mapping();
            fkyaml::node prototypes = fkyaml::node::sequence();
            for (const auto& group : geo.instances()) {
                fkyaml::node entry = fkyaml::node::mapping();
                entry["name"] = fkyaml::node(group.prototype);
                entry[as_mesh ? "mesh" : "object"] = fkyaml::node(group.prototype);
                if (ctx.get<bool>("collision", false)) entry["collision"] = fkyaml::node(std::string("mesh"));
                const float draw = ctx.get<float>("draw_distance", 0.0f);
                if (draw > 0.0f) entry["draw_distance"] = fkyaml::node(static_cast<double>(draw));
                prototypes.as_seq().push_back(std::move(entry));
            }
            props["prototypes"] = std::move(prototypes);
            ctx.set_output("component", make_component(ComponentSpec("InstancePainter", std::move(props))));
        }});

    register_node_type(NodeType{
        "ObjectOutput", "Output", "Object Output",
        "Names an object, gives it geometry and components, and nests children under it",
        NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("name", types().string_t, std::string());
            b.input("geometry", types().geometry_t);
            b.input_default("position", types().vector_t, glm::vec3(0.0f));
            b.input_default("rotation", types().rotation_t, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            b.input_default("scale", types().vector_t, glm::vec3(1.0f));
            b.multi_input("components", types().component_t);
            b.multi_input("children", types().object_spec_t);
            b.output("object", types().object_spec_t);
        },
        [](EvalContext& ctx) {
            auto data = std::make_shared<ObjectSpec::Data>();
            data->name = ctx.get<std::string>("name", std::string());
            if (data->name.empty()) data->name = ctx.props().str("name", "Object");
            data->geometry = detail::geometry_in(ctx, "geometry");
            data->position = ctx.get<glm::vec3>("position", glm::vec3(0.0f));
            data->rotation = ctx.get<glm::quat>("rotation", glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
            data->scale    = ctx.get<glm::vec3>("scale", glm::vec3(1.0f));

            const auto& components = ctx.input("components");
            data->components.reserve(components.count);
            for (uint32_t i = 0; i < components.count; ++i) {
                static const ComponentSpec none;
                const ComponentSpec spec = components.at(i).as<ComponentSpec>(none);
                // An unconnected multi-input link still produces a default-constructed value;
                // skipping the empty ones keeps a graph from emitting a nameless component.
                if (!spec.type.empty()) data->components.push_back(spec);
            }

            const auto& children = ctx.input("children");
            data->children.reserve(children.count);
            for (uint32_t i = 0; i < children.count; ++i) {
                static const ObjectSpec none;
                const ObjectSpec child = children.at(i).as<ObjectSpec>(none);
                if (!child.empty()) data->children.push_back(child);
            }

            const std::string collider = ctx.props().str("collider", "none");
            data->collider = collider == "trimesh" || collider == "mesh" ? 1
                           : (collider == "convex" ? 2 : 0);

            ObjectSpec out;
            out.data = std::move(data);
            ctx.set_output("object", make_object_spec(std::move(out)));
        }});
}

} // namespace procgraph
} // namespace toy
