/**
 * @file nodes_utility.cpp
 * @brief Arithmetic, comparison and routing -- the glue every graph is mostly made of.
 *
 * All `Trivial`, so the compiler fuses runs of them into one job: a Math node costs a few
 * nanoseconds and scheduling one costs far more than that.
 */

#include <toyengine/procgraph/nodes.h>

#include <algorithm>
#include <cmath>

namespace toy {
namespace procgraph {
namespace {

/// @brief The scalar operations `Math` offers, named as the `operation:` property spells them.
float apply_math(const std::string& op, float a, float b, bool& unknown) {
    unknown = false;
    if (op == "add") return a + b;
    if (op == "subtract") return a - b;
    if (op == "multiply") return a * b;
    if (op == "divide") return b != 0.0f ? a / b : 0.0f;   // Zero rather than inf: a NaN would
                                                            // poison every downstream vertex.
    if (op == "power") return std::pow(a, b);
    if (op == "minimum") return std::min(a, b);
    if (op == "maximum") return std::max(a, b);
    if (op == "modulo") return b != 0.0f ? std::fmod(a, b) : 0.0f;
    if (op == "absolute") return std::abs(a);
    if (op == "floor") return std::floor(a);
    if (op == "ceil") return std::ceil(a);
    if (op == "round") return std::round(a);
    if (op == "sqrt") return a > 0.0f ? std::sqrt(a) : 0.0f;
    if (op == "sine") return std::sin(a);
    if (op == "cosine") return std::cos(a);
    if (op == "tangent") return std::tan(a);
    if (op == "arctan2") return std::atan2(a, b);
    if (op == "sign") return a > 0.0f ? 1.0f : (a < 0.0f ? -1.0f : 0.0f);
    if (op == "fraction") return a - std::floor(a);
    if (op == "snap") return b != 0.0f ? std::floor(a / b) * b : a;
    unknown = true;
    return 0.0f;
}

} // namespace

void register_utility_nodes() {
    using coopa::nodegraph::register_node_type;

    register_node_type(NodeType{
        "Math", "Utility", "Math", "Arithmetic on two numbers", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("a", types().float_t, 0.0f);
            b.input_default("b", types().float_t, 0.0f);
            b.output("result", types().float_t);
        },
        [](EvalContext& ctx) {
            bool unknown = false;
            const std::string op = ctx.props().str("operation", "add");
            const float r = apply_math(op, ctx.get<float>("a", 0.0f), ctx.get<float>("b", 0.0f),
                                       unknown);
            if (unknown) ctx.warn("unknown Math operation '" + op + "'; result is 0");
            ctx.set_output("result", make_float(r));
        }});

    register_node_type(NodeType{
        "VectorMath", "Utility", "Vector Math", "Arithmetic on two vectors", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("a", types().vector_t, glm::vec3(0.0f));
            b.input_default("b", types().vector_t, glm::vec3(0.0f));
            b.input_default("scale", types().float_t, 1.0f);
            b.output("vector", types().vector_t);
            b.output("value", types().float_t);
        },
        [](EvalContext& ctx) {
            const glm::vec3 a = ctx.get<glm::vec3>("a", glm::vec3(0.0f));
            const glm::vec3 b = ctx.get<glm::vec3>("b", glm::vec3(0.0f));
            const float s = ctx.get<float>("scale", 1.0f);
            const std::string op = ctx.props().str("operation", "add");

            glm::vec3 vector(0.0f);
            float value = 0.0f;
            if (op == "add") vector = a + b;
            else if (op == "subtract") vector = a - b;
            else if (op == "multiply") vector = a * b;
            else if (op == "divide") {
                for (int i = 0; i < 3; ++i) vector[i] = b[i] != 0.0f ? a[i] / b[i] : 0.0f;
            }
            else if (op == "scale") vector = a * s;
            else if (op == "cross") vector = glm::cross(a, b);
            else if (op == "normalize") {
                const float len = glm::length(a);
                vector = len > 1e-12f ? a / len : glm::vec3(0.0f);
            }
            else if (op == "minimum") vector = glm::min(a, b);
            else if (op == "maximum") vector = glm::max(a, b);
            else if (op == "absolute") vector = glm::abs(a);
            else if (op == "floor") vector = glm::floor(a);
            else if (op == "dot") value = glm::dot(a, b);
            else if (op == "length") value = glm::length(a);
            else if (op == "distance") value = glm::length(a - b);
            else ctx.warn("unknown Vector Math operation '" + op + "'");
            ctx.set_output("vector", make_vector(vector));
            ctx.set_output("value", make_float(value));
        }});

    register_node_type(NodeType{
        "CombineXYZ", "Utility", "Combine XYZ", {}, NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("x", types().float_t, 0.0f);
            b.input_default("y", types().float_t, 0.0f);
            b.input_default("z", types().float_t, 0.0f);
            b.output("vector", types().vector_t);
        },
        [](EvalContext& ctx) {
            ctx.set_output("vector", make_vector(glm::vec3(ctx.get<float>("x", 0.0f),
                                                           ctx.get<float>("y", 0.0f),
                                                           ctx.get<float>("z", 0.0f))));
        }});

    register_node_type(NodeType{
        "SeparateXYZ", "Utility", "Separate XYZ", {}, NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("vector", types().vector_t, glm::vec3(0.0f));
            b.output("x", types().float_t);
            b.output("y", types().float_t);
            b.output("z", types().float_t);
        },
        [](EvalContext& ctx) {
            const glm::vec3 v = ctx.get<glm::vec3>("vector", glm::vec3(0.0f));
            ctx.set_output("x", make_float(v.x));
            ctx.set_output("y", make_float(v.y));
            ctx.set_output("z", make_float(v.z));
        }});

    register_node_type(NodeType{
        "MapRange", "Utility", "Map Range", "Rescales a number from one range to another",
        NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("value", types().float_t, 0.0f);
            b.input_default("from_min", types().float_t, 0.0f);
            b.input_default("from_max", types().float_t, 1.0f);
            b.input_default("to_min", types().float_t, 0.0f);
            b.input_default("to_max", types().float_t, 1.0f);
            b.input_default("clamp", types().bool_t, true);
            b.output("result", types().float_t);
        },
        [](EvalContext& ctx) {
            const float v = ctx.get<float>("value", 0.0f);
            const float f0 = ctx.get<float>("from_min", 0.0f);
            const float f1 = ctx.get<float>("from_max", 1.0f);
            const float t0 = ctx.get<float>("to_min", 0.0f);
            const float t1 = ctx.get<float>("to_max", 1.0f);
            // A zero-width source range has no meaningful mapping; the low end of the target is
            // the least surprising answer and avoids a division by zero.
            float t = (f1 - f0) != 0.0f ? (v - f0) / (f1 - f0) : 0.0f;
            if (ctx.get<bool>("clamp", true)) t = std::clamp(t, 0.0f, 1.0f);
            ctx.set_output("result", make_float(t0 + (t1 - t0) * t));
        }});

    register_node_type(NodeType{
        "Clamp", "Utility", "Clamp", {}, NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("value", types().float_t, 0.0f);
            b.input_default("min", types().float_t, 0.0f);
            b.input_default("max", types().float_t, 1.0f);
            b.output("result", types().float_t);
        },
        [](EvalContext& ctx) {
            const float lo = ctx.get<float>("min", 0.0f);
            const float hi = ctx.get<float>("max", 1.0f);
            const float v = ctx.get<float>("value", 0.0f);
            ctx.set_output("result", make_float(hi >= lo ? std::clamp(v, lo, hi)
                                                         : std::clamp(v, hi, lo)));
        }});

    register_node_type(NodeType{
        "Mix", "Utility", "Mix", "Blends between two values", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag& props) {
            // A dynamic declaration: the sockets take whatever type `data_type` names, which is
            // what lets one node mix floats, vectors or colours.
            const std::string which = props.str("data_type", "Float");
            coopa::nodegraph::TypeId t = coopa::nodegraph::TypeRegistry::instance().find(which);
            if (t == coopa::nodegraph::k_invalid_type) t = types().float_t;
            b.input_default("factor", types().float_t, 0.5f);
            b.input("a", t);
            b.input("b", t);
            b.output("result", t);
        },
        [](EvalContext& ctx) {
            const float f = std::clamp(ctx.get<float>("factor", 0.5f), 0.0f, 1.0f);
            const auto& a = ctx.input_value("a");
            const auto& b = ctx.input_value("b");
            if (a.type() == types().vector_t) {
                ctx.set_output("result", make_vector(glm::mix(a.as<glm::vec3>(glm::vec3(0.0f)),
                                                              b.as<glm::vec3>(glm::vec3(0.0f)), f)));
            } else if (a.type() == types().color_t) {
                ctx.set_output("result", make_color(glm::mix(a.as<glm::vec4>(glm::vec4(0.0f)),
                                                             b.as<glm::vec4>(glm::vec4(0.0f)), f)));
            } else {
                ctx.set_output("result", make_float(glm::mix(a.as<float>(0.0f), b.as<float>(0.0f), f)));
            }
        }});

    register_node_type(NodeType{
        "Compare", "Utility", "Compare", "Tests two numbers", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("a", types().float_t, 0.0f);
            b.input_default("b", types().float_t, 0.0f);
            b.input_default("epsilon", types().float_t, 1e-4f);
            b.output("result", types().bool_t);
        },
        [](EvalContext& ctx) {
            const float a = ctx.get<float>("a", 0.0f);
            const float b = ctx.get<float>("b", 0.0f);
            const float eps = ctx.get<float>("epsilon", 1e-4f);
            const std::string op = ctx.props().str("operation", "greater_than");
            bool r = false;
            if (op == "greater_than") r = a > b;
            else if (op == "less_than") r = a < b;
            else if (op == "greater_equal") r = a >= b;
            else if (op == "less_equal") r = a <= b;
            else if (op == "equal") r = std::abs(a - b) <= eps;
            else if (op == "not_equal") r = std::abs(a - b) > eps;
            else ctx.warn("unknown Compare operation '" + op + "'");
            ctx.set_output("result", make_bool(r));
        }});

    register_node_type(NodeType{
        "BooleanMath", "Utility", "Boolean Math", {}, NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("a", types().bool_t, false);
            b.input_default("b", types().bool_t, false);
            b.output("result", types().bool_t);
        },
        [](EvalContext& ctx) {
            const bool a = ctx.get<bool>("a", false);
            const bool b = ctx.get<bool>("b", false);
            const std::string op = ctx.props().str("operation", "and");
            bool r = false;
            if (op == "and") r = a && b;
            else if (op == "or") r = a || b;
            else if (op == "not") r = !a;
            else if (op == "xor") r = a != b;
            else ctx.warn("unknown Boolean Math operation '" + op + "'");
            ctx.set_output("result", make_bool(r));
        }});

    register_node_type(NodeType{
        "Switch", "Utility", "Switch", "Picks one of two inputs", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag& props) {
            const std::string which = props.str("data_type", "Geometry");
            coopa::nodegraph::TypeId t = coopa::nodegraph::TypeRegistry::instance().find(which);
            if (t == coopa::nodegraph::k_invalid_type) t = types().geometry_t;
            b.input_default("condition", types().bool_t, false);
            // Both branches are evaluated: this evaluator is eager, and the library's lazy-input
            // machinery is declared but not yet deferring (see its README's Limits). For geometry
            // that is wasted work, not wrong work -- the unused branch is simply discarded.
            b.input("false", t);
            b.input("true", t);
            b.output("output", t);
        },
        [](EvalContext& ctx) {
            const bool c = ctx.get<bool>("condition", false);
            ctx.set_output("output", ctx.input_value(c ? "true" : "false"));
        }});

    register_node_type(NodeType{
        "RotationFromEuler", "Utility", "Rotation from Euler",
        "Builds a rotation from degrees about X, Y and Z", NodeCost::Trivial,
        [](DeclarationBuilder& b, const PropertyBag&) {
            b.input_default("degrees", types().vector_t, glm::vec3(0.0f));
            b.output("rotation", types().rotation_t);
        },
        [](EvalContext& ctx) {
            const glm::vec3 deg = ctx.get<glm::vec3>("degrees", glm::vec3(0.0f));
            ctx.set_output("rotation", make_rotation(glm::quat(glm::radians(deg))));
        }});
}

} // namespace procgraph
} // namespace toy
