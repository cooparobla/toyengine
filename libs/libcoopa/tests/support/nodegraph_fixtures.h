/**
 * @file nodegraph_fixtures.h
 * @brief Socket types and node types shared by the nodegraph suites.
 *
 * The library knows no types of its own, so every suite needs a domain to test against. This is
 * the smallest one that exercises each mechanism: a scalar, a small vector that still fits a
 * Value's inline buffer, a payload that is deliberately too big for it, and a type that counts
 * its own copies so a test can prove the evaluator does not deep-copy on fan-out.
 *
 * Registration is idempotent -- the whole suite runs in one process against the process-wide
 * registries, so every `COOPA_TEST` calls `types()` and gets the same ids back.
 */

#ifndef COOPA_TESTS_SUPPORT_NODEGRAPH_FIXTURES_H
#define COOPA_TESTS_SUPPORT_NODEGRAPH_FIXTURES_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/nodegraph/compile.h>
#include <coopa/nodegraph/evaluator.h>
#include <coopa/nodegraph/graph.h>
#include <coopa/nodegraph/node_type.h>
#include <coopa/nodegraph/type_registry.h>
#include <coopa/nodegraph/value.h>

namespace nodegraph_fixtures {

using namespace coopa::nodegraph;

/// @brief Larger than Value::k_inline_bytes, so it is forced onto the shared-block path.
struct BigPayload {
    std::vector<float> data;
    int                tag = 0;
    /// @brief Pushes the struct comfortably past the inline budget rather than exactly onto it.
    char               padding[64] = {};

    BigPayload() = default;
    explicit BigPayload(int t, size_t n = 64) : data(n, static_cast<float>(t)), tag(t) {}
    bool operator==(const BigPayload& o) const { return tag == o.tag && data == o.data; }
};
static_assert(sizeof(BigPayload) > coopa::nodegraph::Value::k_inline_bytes,
              "BigPayload must not fit inline, or it stops testing the shared-block path");

/// @brief Counts its own deep copies process-wide, so fan-out behaviour is measurable.
struct Tracked {
    int tag = 0;

    Tracked() = default;
    explicit Tracked(int t) : tag(t) {}
    Tracked(const Tracked& o) : tag(o.tag) { copies().fetch_add(1, std::memory_order_relaxed); }
    Tracked& operator=(const Tracked& o) {
        tag = o.tag;
        copies().fetch_add(1, std::memory_order_relaxed);
        return *this;
    }
    Tracked(Tracked&&) noexcept = default;
    Tracked& operator=(Tracked&&) noexcept = default;
    bool operator==(const Tracked& o) const { return tag == o.tag; }

    /// @brief Deep-copy counter. A refcount bump on a shared Value must NOT touch this.
    static std::atomic<int>& copies() {
        static std::atomic<int> n{0};
        return n;
    }
    /// @brief Padded well past the inline budget so a Tracked always lives in a shared block.
    char padding[64] = {};
};

static_assert(sizeof(Tracked) > coopa::nodegraph::Value::k_inline_bytes,
              "Tracked must not fit inline, or the fan-out copy test proves nothing");

/// @brief A shared empty Tracked, for an `as<T>(fallback)` call site.
inline const Tracked& empty_tracked_();

/// @brief The ids registered by `types()`.
struct TypeIds {
    TypeId f = k_invalid_type;    ///< float
    TypeId i = k_invalid_type;    ///< int32_t
    TypeId b = k_invalid_type;    ///< bool
    TypeId v = k_invalid_type;    ///< glm::vec3 (fits inline)
    TypeId big = k_invalid_type;  ///< BigPayload (does not)
    TypeId tracked = k_invalid_type;
};

/// @brief Registers the fixture's socket types and conversions. Idempotent.
inline const TypeIds& types() {
    static const TypeIds ids = [] {
        auto& reg = TypeRegistry::instance();
        TypeIds t;
        t.f       = reg.add<float>("Float");
        t.i       = reg.add<int32_t>("Int");
        t.b       = reg.add<bool>("Bool");
        t.v       = reg.add<glm::vec3>("Vector");
        t.big     = reg.add<BigPayload>("Big");
        t.tracked = reg.add<Tracked>("Tracked");

        // glm::vec3 has no operator== that returns bool, so equality and hashing are supplied.
        reg.set_equals(t.v, [](const void* a, const void* b) {
            return *static_cast<const glm::vec3*>(a) == *static_cast<const glm::vec3*>(b);
        });
        reg.set_hash(t.v, [](const void* p, uint64_t& seed) {
            hash_bytes(p, sizeof(glm::vec3), seed);
        });
        reg.set_hash(t.f, [](const void* p, uint64_t& seed) { hash_bytes(p, sizeof(float), seed); });
        reg.set_hash(t.i, [](const void* p, uint64_t& seed) { hash_bytes(p, sizeof(int32_t), seed); });
        reg.set_hash(t.b, [](const void* p, uint64_t& seed) { hash_bytes(p, sizeof(bool), seed); });

        // YAML hooks, so socket defaults round-trip through a file.
        reg.set_yaml_hooks(t.f,
            [](const fkyaml::node& n, void* dst) {
                if (!n.is_float_number() && !n.is_integer()) return false;
                *static_cast<float*>(dst) = static_cast<float>(
                    n.is_integer() ? static_cast<double>(n.get_value<int64_t>()) : n.get_value<double>());
                return true;
            },
            [](const void* src, fkyaml::node& out) {
                out = fkyaml::node(static_cast<double>(*static_cast<const float*>(src)));
                return true;
            });
        reg.set_yaml_hooks(t.i,
            [](const fkyaml::node& n, void* dst) {
                if (!n.is_integer()) return false;
                *static_cast<int32_t*>(dst) = static_cast<int32_t>(n.get_value<int64_t>());
                return true;
            },
            [](const void* src, fkyaml::node& out) {
                out = fkyaml::node(static_cast<int64_t>(*static_cast<const int32_t*>(src)));
                return true;
            });
        reg.set_yaml_hooks(t.v,
            [](const fkyaml::node& n, void* dst) {
                if (!n.is_sequence() || n.size() < 3) return false;
                auto* o = static_cast<glm::vec3*>(dst);
                for (int k = 0; k < 3; ++k) (*o)[k] = static_cast<float>(n[k].get_value<double>());
                return true;
            },
            [](const void* src, fkyaml::node& out) {
                const auto* v = static_cast<const glm::vec3*>(src);
                out = fkyaml::node::sequence();
                for (int k = 0; k < 3; ++k) out.as_seq().push_back(fkyaml::node(static_cast<double>((*v)[k])));
                return true;
            });

        // Implicit conversions, mirroring Blender's Int -> Float -> Vector widening.
        reg.add_conversion(t.i, t.f, [](const void* s, void* d) {
            *static_cast<float*>(d) = static_cast<float>(*static_cast<const int32_t*>(s));
        });
        reg.add_conversion(t.f, t.v, [](const void* s, void* d) {
            float f = *static_cast<const float*>(s);
            *static_cast<glm::vec3*>(d) = glm::vec3(f, f, f);
        });
        reg.add_conversion(t.f, t.i, [](const void* s, void* d) {
            *static_cast<int32_t*>(d) = static_cast<int32_t>(*static_cast<const float*>(s));
        });
        return t;
    }();
    return ids;
}

/// @brief Counts how many times each fixture node body ran, so fusion/pruning is observable.
inline std::atomic<int>& eval_count(const std::string& which) {
    static std::vector<std::pair<std::string, std::atomic<int>*>> table;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [name, counter] : table) {
        if (name == which) return *counter;
    }
    table.emplace_back(which, new std::atomic<int>(0));
    return *table.back().second;
}

/// @brief Zeroes every counter and the Tracked copy count. Call at the top of a test.
inline void reset_counters() {
    for (const char* n : {"const", "add", "mul", "heavy", "sum", "throw", "passthrough", "slow"}) {
        eval_count(n).store(0, std::memory_order_relaxed);
    }
    Tracked::copies().store(0, std::memory_order_relaxed);
}

/// @brief Registers the fixture's node types. Idempotent.
inline void register_nodes() {
    static bool done = [] {
        const TypeIds& t = types();
        auto& reg = NodeTypeRegistry::instance();

        // Const: a literal from its own props. No inputs, so it is always a chain head.
        reg.add(NodeType{"Const", "Input", "Const", "A literal float", NodeCost::Trivial,
            [](DeclarationBuilder& b, const PropertyBag&) { b.output("value", types().f); },
            [](EvalContext& ctx) {
                eval_count("const").fetch_add(1, std::memory_order_relaxed);
                ctx.set("value", static_cast<float>(ctx.props().num("value", 0.0)));
            }});

        // Add / Mul: the cheap arithmetic that fusion is supposed to collapse.
        reg.add(NodeType{"Add", "Utility", "Add", {}, NodeCost::Trivial,
            [](DeclarationBuilder& b, const PropertyBag&) {
                b.input_default("a", types().f, 0.0f);
                b.input_default("b", types().f, 0.0f);
                b.output("result", types().f);
            },
            [](EvalContext& ctx) {
                eval_count("add").fetch_add(1, std::memory_order_relaxed);
                ctx.set("result", ctx.get<float>("a") + ctx.get<float>("b"));
            }});

        reg.add(NodeType{"Mul", "Utility", "Multiply", {}, NodeCost::Trivial,
            [](DeclarationBuilder& b, const PropertyBag&) {
                b.input_default("a", types().f, 0.0f);
                b.input_default("b", types().f, 1.0f);
                b.output("result", types().f);
            },
            [](EvalContext& ctx) {
                eval_count("mul").fetch_add(1, std::memory_order_relaxed);
                ctx.set("result", ctx.get<float>("a") * ctx.get<float>("b"));
            }});

        // Heavy: declared expensive, so the compiler must give it its own chain.
        reg.add(NodeType{"Heavy", "Utility", "Heavy", {}, NodeCost::Heavy,
            [](DeclarationBuilder& b, const PropertyBag&) {
                b.input_default("a", types().f, 0.0f);
                b.output("result", types().f);
            },
            [](EvalContext& ctx) {
                eval_count("heavy").fetch_add(1, std::memory_order_relaxed);
                ctx.set("result", ctx.get<float>("a") * 2.0f);
            }});

        // Sum: a multi-input, which is the one shape a plain socket cannot express.
        reg.add(NodeType{"Sum", "Utility", "Sum", {}, NodeCost::Cheap,
            [](DeclarationBuilder& b, const PropertyBag&) {
                b.multi_input("values", types().f);
                b.output("result", types().f);
                b.output("count", types().i);
            },
            [](EvalContext& ctx) {
                eval_count("sum").fetch_add(1, std::memory_order_relaxed);
                const InputView& in = ctx.input("values");
                float total = 0.0f;
                for (uint32_t i = 0; i < in.count; ++i) total += in.at(i).as<float>(0.0f);
                ctx.set("result", total);
                ctx.set("count", static_cast<int32_t>(in.count));
            }});

        // MakeVec: proves an output of a different type, and feeds the Float->Vector conversion.
        reg.add(NodeType{"MakeVec", "Utility", "Make Vector", {}, NodeCost::Trivial,
            [](DeclarationBuilder& b, const PropertyBag&) {
                b.input_default("x", types().f, 0.0f);
                b.output("vector", types().v);
            },
            [](EvalContext& ctx) {
                float x = ctx.get<float>("x");
                ctx.set("vector", glm::vec3(x, x * 2.0f, x * 3.0f));
            }});

        reg.add(NodeType{"VecLength", "Utility", "Vector Length", {}, NodeCost::Trivial,
            [](DeclarationBuilder& b, const PropertyBag&) {
                b.input_default("vector", types().v, glm::vec3(0.0f));
                b.output("length", types().f);
            },
            [](EvalContext& ctx) {
                glm::vec3 v = ctx.get<glm::vec3>("vector", glm::vec3(0.0f));
                ctx.set("length", glm::length(v));
            }});

        // Thrower: proves one bad node is reported rather than taking the evaluation down.
        reg.add(NodeType{"Throw", "Utility", "Throw", {}, NodeCost::Cheap,
            [](DeclarationBuilder& b, const PropertyBag&) {
                b.input_default("a", types().f, 0.0f);
                b.output("result", types().f);
            },
            [](EvalContext&) {
                eval_count("throw").fetch_add(1, std::memory_order_relaxed);
                throw std::runtime_error("deliberate fixture failure");
            }});

        // MakeTracked / ReadTracked: the fan-out copy test's producer and consumer.
        reg.add(NodeType{"MakeTracked", "Input", "Make Tracked", {}, NodeCost::Cheap,
            [](DeclarationBuilder& b, const PropertyBag&) { b.output("value", types().tracked); },
            [](EvalContext& ctx) {
                ctx.set_output("value", Value::make(types().tracked,
                                                    Tracked(static_cast<int>(ctx.props().integer("tag", 1)))));
            }});

        reg.add(NodeType{"ReadTracked", "Utility", "Read Tracked", {}, NodeCost::Cheap,
            [](DeclarationBuilder& b, const PropertyBag&) {
                b.input("value", types().tracked);
                b.output("tag", types().i);
            },
            [](EvalContext& ctx) {
                eval_count("passthrough").fetch_add(1, std::memory_order_relaxed);
                const Tracked& tr = ctx.input_value("value").as<Tracked>(empty_tracked_());
                ctx.set("tag", static_cast<int32_t>(tr.tag));
            }});

        // BigSource / BigSink: proves a large payload's slot is released at its last read.
        reg.add(NodeType{"BigSource", "Input", "Big Source", {}, NodeCost::Cheap,
            [](DeclarationBuilder& b, const PropertyBag&) { b.output("payload", types().big); },
            [](EvalContext& ctx) {
                ctx.set_output("payload", Value::make(types().big,
                                                       BigPayload(static_cast<int>(ctx.props().integer("tag", 7)))));
            }});

        reg.add(NodeType{"BigSink", "Utility", "Big Sink", {}, NodeCost::Cheap,
            [](DeclarationBuilder& b, const PropertyBag&) {
                b.input("payload", types().big);
                b.output("tag", types().i);
            },
            [](EvalContext& ctx) {
                static const BigPayload nothing;
                ctx.set("tag", static_cast<int32_t>(ctx.input_value("payload").as<BigPayload>(nothing).tag));
            }});

        // Slow: sleeps, so the parallel path demonstrably overlaps independent work.
        reg.add(NodeType{"Slow", "Utility", "Slow", {}, NodeCost::Heavy,
            [](DeclarationBuilder& b, const PropertyBag&) {
                b.input_default("a", types().f, 0.0f);
                b.output("result", types().f);
            },
            [](EvalContext& ctx) {
                eval_count("slow").fetch_add(1, std::memory_order_relaxed);
                auto ms = static_cast<int>(ctx.props().integer("ms", 20));
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                ctx.set("result", ctx.get<float>("a") + 1.0f);
            }});

        // A dynamic declaration: the socket type follows a property, as a Switch node's would.
        reg.add(NodeType{"Passthrough", "Utility", "Passthrough", {}, NodeCost::Trivial,
            [](DeclarationBuilder& b, const PropertyBag& props) {
                const std::string which = props.str("data_type", "Float");
                TypeId t = TypeRegistry::instance().find(which);
                if (t == k_invalid_type) t = types().f;
                b.input("value", t);
                b.output("value", t);
            },
            [](EvalContext& ctx) {
                ctx.set_output("value", ctx.input_value("value"));
            }});

        (void)t;
        return true;
    }();
    (void)done;
}

inline const Tracked& empty_tracked_() {
    static const Tracked nothing;
    return nothing;
}

/// @brief Registers both halves. Every nodegraph test starts with this.
inline const TypeIds& setup() {
    const TypeIds& t = types();
    register_nodes();
    return t;
}

/// @brief A float Value, the fixture's most common literal.
inline Value fval(float v) { return Value::make(types().f, v); }
/// @brief An int Value.
inline Value ival(int32_t v) { return Value::make(types().i, v); }

/// @brief Adds a Const node carrying `v`.
inline NodeId add_const(Graph& g, float v) {
    PropertyBag props;
    props.set("value", static_cast<double>(v));
    return g.add_node("Const", std::move(props));
}

/// @brief Declares one interface output of type Float named `id`, wired from `from`.
inline void wire_float_output(Graph& g, const std::string& id, NodeId from, const std::string& socket) {
    InterfaceSocket os;
    os.identifier = id;
    os.type       = types().f;
    g.outputs().push_back(os);
    NodeId out = g.find_node_of_type(k_group_output_type);
    if (out == 0) out = g.add_node(k_group_output_type);
    g.add_link(SocketRef{from, socket}, SocketRef{out, id});
}

} // namespace nodegraph_fixtures

#endif // COOPA_TESTS_SUPPORT_NODEGRAPH_FIXTURES_H
