/**
 * @file declaration.h
 * @brief What sockets a node has -- decided per node instance, not per node type.
 *
 * ### Why declaration is a function
 *
 * A static socket list cannot express the nodes that make a graph system worth having. A Switch
 * node's sockets take whatever type its `data_type` property names. A Group node's sockets come
 * from the graph it references. A Join node takes an unbounded number of links on one socket. All
 * three are ordinary, and all three need the socket list to be computed from the node's own
 * settings -- so `NodeType::declare` is a function over a `PropertyBag`, and the compiler runs it
 * once per node before it does anything else.
 *
 * Everything downstream of that point works on `NodeDeclaration`s, never on node types. That is
 * what keeps the compiler and the evaluator from having to know that dynamic sockets exist.
 *
 * ### Identifiers are the stable key
 *
 * A link stores a socket's `identifier`, never its index, because inserting a socket into a node
 * type would silently re-point every link in every saved graph. `label` is the display string and
 * may be changed freely.
 */

#ifndef COOPA_NODEGRAPH_DECLARATION_H
#define COOPA_NODEGRAPH_DECLARATION_H

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fkYAML/node.hpp>

#include <coopa/nodegraph/type_registry.h>
#include <coopa/nodegraph/value.h>

namespace coopa {
namespace nodegraph {

/// @brief Per-socket behaviour flags. Combined with the operators below.
enum class SocketFlags : uint32_t {
    None       = 0,
    MultiInput = 1u << 0, ///< Accepts any number of links; the node reads them in link order.
    Lazy       = 1u << 1, ///< Not evaluated up front; the node asks for it via EvalContext::request().
    Hide       = 1u << 2, ///< Authored but not offered in a UI.
};

inline SocketFlags operator|(SocketFlags a, SocketFlags b) {
    return static_cast<SocketFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline SocketFlags& operator|=(SocketFlags& a, SocketFlags b) { a = a | b; return a; }
inline bool has_flag(SocketFlags set, SocketFlags f) {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(f)) != 0;
}

/**
 * @struct SocketDecl
 * @brief One declared input or output.
 */
struct SocketDecl {
    std::string identifier;                 ///< The stable key links and YAML use. Never renamed.
    std::string label;                      ///< Display string; falls back to `identifier`.
    TypeId      type = k_invalid_type;
    Value       default_value;              ///< Used when nothing is linked. May be empty.
    SocketFlags flags = SocketFlags::None;

    bool multi_input() const { return has_flag(flags, SocketFlags::MultiInput); }
    bool lazy() const { return has_flag(flags, SocketFlags::Lazy); }
    const std::string& display() const { return label.empty() ? identifier : label; }
};

/**
 * @struct NodeDeclaration
 * @brief One node instance's full socket list, as computed by its type's `declare`.
 */
struct NodeDeclaration {
    std::vector<SocketDecl> inputs;
    std::vector<SocketDecl> outputs;

    /// @brief Index of the input named `id`, or -1.
    int find_input(std::string_view id) const {
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (inputs[i].identifier == id) return static_cast<int>(i);
        }
        return -1;
    }

    /// @brief Index of the output named `id`, or -1.
    int find_output(std::string_view id) const {
        for (size_t i = 0; i < outputs.size(); ++i) {
            if (outputs[i].identifier == id) return static_cast<int>(i);
        }
        return -1;
    }
};

/**
 * @class PropertyBag
 * @brief A node's non-socket settings: an enum choice, a mode, a referenced asset name.
 *
 * Backed by an `fkyaml::node` rather than a typed struct, for one reason that matters: a key this
 * build does not understand is **round-tripped untouched**. A graph authored against a newer node
 * type therefore loads, saves and reloads without silently dropping the setting it could not
 * interpret -- the same guarantee the editor's component inspector gives scene YAML.
 */
class PropertyBag {
public:
    PropertyBag() : node_(fkyaml::node::mapping()) {}
    explicit PropertyBag(fkyaml::node node)
        : node_(node.is_mapping() ? std::move(node) : fkyaml::node::mapping()) {}

    /// @brief The string at `key`, or `fallback`.
    std::string str(std::string_view key, std::string fallback = {}) const {
        const fkyaml::node* n = find_(key);
        if (n == nullptr) return fallback;
        if (n->is_string()) return n->get_value<std::string>();
        return fallback;
    }

    /// @brief The number at `key` as a double, or `fallback`. Accepts ints and floats.
    double num(std::string_view key, double fallback = 0.0) const {
        const fkyaml::node* n = find_(key);
        if (n == nullptr) return fallback;
        if (n->is_float_number()) return n->get_value<double>();
        if (n->is_integer()) return static_cast<double>(n->get_value<int64_t>());
        return fallback;
    }

    /// @brief The integer at `key`, or `fallback`.
    int64_t integer(std::string_view key, int64_t fallback = 0) const {
        const fkyaml::node* n = find_(key);
        if (n == nullptr) return fallback;
        if (n->is_integer()) return n->get_value<int64_t>();
        if (n->is_float_number()) return static_cast<int64_t>(n->get_value<double>());
        return fallback;
    }

    /// @brief The boolean at `key`, or `fallback`.
    bool boolean(std::string_view key, bool fallback = false) const {
        const fkyaml::node* n = find_(key);
        if (n == nullptr) return fallback;
        if (n->is_boolean()) return n->get_value<bool>();
        return fallback;
    }

    bool contains(std::string_view key) const { return find_(key) != nullptr; }

    void set(std::string_view key, fkyaml::node v) { node_[std::string(key)] = std::move(v); }
    void set(std::string_view key, const std::string& v) { node_[std::string(key)] = fkyaml::node(v); }
    void set(std::string_view key, double v) { node_[std::string(key)] = fkyaml::node(v); }
    void set(std::string_view key, int64_t v) { node_[std::string(key)] = fkyaml::node(v); }
    void set(std::string_view key, bool v) { node_[std::string(key)] = fkyaml::node(v); }

    /// @brief The backing node, including keys this build does not interpret.
    const fkyaml::node& raw() const { return node_; }
    fkyaml::node&       raw() { return node_; }

    bool empty() const { return !node_.is_mapping() || node_.size() == 0; }

    /// @brief Folds every key and scalar value into `seed`, so a settings change is detectable.
    void hash_into(uint64_t& seed) const { hash_node_(node_, seed); }

private:
    const fkyaml::node* find_(std::string_view key) const {
        if (!node_.is_mapping()) return nullptr;
        const std::string k(key);
        if (!node_.contains(k)) return nullptr;
        return &node_.at(k);
    }

    static void fold_(uint64_t& seed, uint64_t v) {
        seed ^= v;
        seed *= 1099511628211ull;
    }

    static void hash_str_(const std::string& s, uint64_t& seed) {
        for (unsigned char c : s) fold_(seed, static_cast<uint64_t>(c));
    }

    static void hash_node_(const fkyaml::node& n, uint64_t& seed) {
        if (n.is_mapping()) {
            // fkYAML keeps mapping order, and a reordered file is the same settings, so the
            // per-key folds are combined with addition rather than in sequence.
            uint64_t acc = 0;
            for (auto it = n.begin(); it != n.end(); ++it) {
                uint64_t sub = 1469598103934665603ull;
                hash_str_(it.key().get_value<std::string>(), sub);
                hash_node_(it.value(), sub);
                acc += sub;
            }
            fold_(seed, acc);
        } else if (n.is_sequence()) {
            for (const auto& child : n) hash_node_(child, seed);
        } else if (n.is_string()) {
            hash_str_(n.get_value<std::string>(), seed);
        } else if (n.is_boolean()) {
            fold_(seed, n.get_value<bool>() ? 1u : 2u);
        } else if (n.is_integer()) {
            fold_(seed, static_cast<uint64_t>(n.get_value<int64_t>()));
        } else if (n.is_float_number()) {
            double d = n.get_value<double>();
            uint64_t bits = 0;
            static_assert(sizeof(d) == sizeof(bits), "double is not 64-bit");
            std::memcpy(&bits, &d, sizeof(bits));
            fold_(seed, bits);
        } else {
            fold_(seed, 0x6e756c6cull);   // "null"
        }
    }

    fkyaml::node node_;
};

/**
 * @class DeclarationBuilder
 * @brief What a `NodeType::declare` function writes into.
 *
 * Each `input()`/`output()` returns a reference so the call site can keep setting fields, which
 * keeps a declaration readable without a builder method per field:
 *
 * @code
 * b.input("a", float_type).default_value = Value::make(float_type, 0.0f);
 * b.output("result", float_type);
 * @endcode
 */
class DeclarationBuilder {
public:
    explicit DeclarationBuilder(NodeDeclaration& out) : out_(out) {}

    SocketDecl& input(std::string identifier, TypeId type) {
        out_.inputs.push_back(SocketDecl{std::move(identifier), {}, type, Value(), SocketFlags::None});
        return out_.inputs.back();
    }

    SocketDecl& output(std::string identifier, TypeId type) {
        out_.outputs.push_back(SocketDecl{std::move(identifier), {}, type, Value(), SocketFlags::None});
        return out_.outputs.back();
    }

    /// @brief An input with a default, the most common shape.
    template <typename T>
    SocketDecl& input_default(std::string identifier, TypeId type, T&& v) {
        SocketDecl& s = input(std::move(identifier), type);
        s.default_value = Value::make(type, std::forward<T>(v));
        return s;
    }

    /// @brief A multi-input: any number of links, read in link order.
    SocketDecl& multi_input(std::string identifier, TypeId type) {
        SocketDecl& s = input(std::move(identifier), type);
        s.flags |= SocketFlags::MultiInput;
        return s;
    }

private:
    NodeDeclaration& out_;
};

} // namespace nodegraph
} // namespace coopa

#endif // COOPA_NODEGRAPH_DECLARATION_H
