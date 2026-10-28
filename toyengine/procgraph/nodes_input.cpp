/**
 * @file nodes_input.cpp
 * @brief Literal and context inputs: the leaves of a graph.
 */

#include <toyengine/procgraph/nodes.h>

#include <toyengine/core/yaml_read.h>

namespace toy {
namespace procgraph {
namespace {

/// @brief Reads a literal from the node's own props, which is where a UI would store it.
void declare_float_literal(DeclarationBuilder& b, const PropertyBag&) {
    b.output("value", types().float_t);
}

} // namespace

void register_input_nodes() {
    using coopa::nodegraph::register_node_type;

    register_node_type(NodeType{
        "Float", "Input", "Float", "A constant number", NodeCost::Trivial,
        declare_float_literal,
        [](EvalContext& ctx) {
            ctx.set_output("value", make_float(static_cast<float>(ctx.props().num("value", 0.0))));
        }});

    register_node_type(NodeType{
        "Integer", "Input", "Integer", "A constant whole number", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) { b.output("value", types().int_t); },
        [](EvalContext& ctx) {
            ctx.set_output("value", make_int(static_cast<int32_t>(ctx.props().integer("value", 0))));
        }});

    register_node_type(NodeType{
        "Boolean", "Input", "Boolean", "A constant true/false", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) { b.output("value", types().bool_t); },
        [](EvalContext& ctx) {
            ctx.set_output("value", make_bool(ctx.props().boolean("value", false)));
        }});

    register_node_type(NodeType{
        "Vector", "Input", "Vector", "A constant 3D vector", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) { b.output("value", types().vector_t); },
        [](EvalContext& ctx) {
            glm::vec3 v(0.0f);
            const fkyaml::node& raw = ctx.props().raw();
            if (raw.is_mapping() && raw.contains("value")) v = core::yaml::as_vec3(raw.at("value"));
            ctx.set_output("value", make_vector(v));
        }});

    register_node_type(NodeType{
        "Color", "Input", "Color", "A constant colour", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) { b.output("value", types().color_t); },
        [](EvalContext& ctx) {
            glm::vec4 c(1.0f);
            const fkyaml::node& raw = ctx.props().raw();
            if (raw.is_mapping() && raw.contains("value")) {
                c = glm::vec4(core::yaml::as_rgb(raw.at("value")), 1.0f);
            }
            ctx.set_output("value", make_color(c));
        }});

    register_node_type(NodeType{
        "String", "Input", "String", "A constant piece of text", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) { b.output("value", types().string_t); },
        [](EvalContext& ctx) {
            ctx.set_output("value", make_string(ctx.props().str("value")));
        }});

    // Variable: the escape hatch, read from the system's variable table rather than from props.
    // Note the graph can ALSO reach a variable through a `{ var: }` input binding; this node is
    // for a variable a graph wants unconditionally rather than one the author wired up.
    register_node_type(NodeType{
        "Variable", "Input", "Variable",
        "Reads a named value the scene or the game set", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag& props) {
            const std::string which = props.str("data_type", "Float");
            coopa::nodegraph::TypeId t = coopa::nodegraph::TypeRegistry::instance().find(which);
            if (t == coopa::nodegraph::k_invalid_type) t = types().float_t;
            // A visible fallback input rather than a hidden default: "what happens when the
            // variable is not set" is the first question an author asks.
            b.input("fallback", t);
            b.output("value", t);
            b.output("found", types().bool_t);
        },
        [](EvalContext& ctx) {
            const std::string name = ctx.props().str("name");
            if (name.empty()) {
                ctx.warn("a Variable node needs a `name` property; using the fallback");
                ctx.set_output("value", ctx.input_value("fallback"));
                ctx.set_output("found", make_bool(false));
                return;
            }
            // Read from the bake's own SNAPSHOT of the table, never from the live system: the
            // bake runs on a worker and set_variable() is called from the main thread.
            const coopa::nodegraph::Value& found = bake_context(ctx).variable(name);
            if (found.empty()) {
                ctx.set_output("value", ctx.input_value("fallback"));
                ctx.set_output("found", make_bool(false));
                return;
            }
            const auto wanted = ctx.declaration().outputs.at(0).type;
            ctx.set_output("value", found.type() == wanted ? found : found.converted_to(wanted));
            ctx.set_output("found", make_bool(true));
        }});

    register_node_type(NodeType{
        "Seed", "Input", "Seed", "This bake's seed, for repeatable randomness", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) { b.output("seed", types().int_t); },
        [](EvalContext& ctx) {
            ctx.set_output("seed", make_int(static_cast<int32_t>(bake_context(ctx).seed)));
        }});

    register_node_type(NodeType{
        "RandomValue", "Input", "Random Value",
        "A repeatable pseudo-random number, derived from the bake seed and an index",
        NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("index", types().int_t, int32_t(0));
            b.input_default("min", types().float_t, 0.0f);
            b.input_default("max", types().float_t, 1.0f);
            b.output("value", types().float_t);
        },
        [](EvalContext& ctx) {
            const auto index = static_cast<uint32_t>(ctx.get<int32_t>("index", 0));
            const float lo = ctx.get<float>("min", 0.0f);
            const float hi = ctx.get<float>("max", 1.0f);
            const float t = detail::hash01(bake_context(ctx).seed, index);
            ctx.set_output("value", make_float(lo + (hi - lo) * t));
        }});
}

} // namespace procgraph
} // namespace toy
