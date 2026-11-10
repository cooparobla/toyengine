/**
 * @file graph.h
 * @brief `Graph` -- the authored form: nodes, links, and a named interface.
 *
 * This is the mutable, single-owner, editable representation. It is what a file holds and what a
 * UI edits, and it is deliberately *not* what gets evaluated: `compile()` turns it into an
 * immutable `CompiledGraph` with groups flattened, conversions inserted and dead nodes dropped.
 * Keeping the two apart is what lets one authored graph be compiled once and evaluated many times
 * from many threads.
 *
 * ### The interface is what makes a graph reusable
 *
 * `inputs()` and `outputs()` are the graph's own named parameters. They serve two purposes at
 * once: they are what a consumer binds values to when it evaluates the graph, and they are the
 * socket list a **Group node** exposes when another graph references this one. One mechanism, so
 * a graph that works standalone works as a group with no extra authoring.
 *
 * ### Node ids
 *
 * Ids are assigned by `add_node()` and never reused within a Graph, so a link can safely hold one.
 * They are stable across a save/load round-trip because they are written to the file. Id 0 is
 * reserved to mean "no node", which is what lets a graph-wide diagnostic use node 0.
 */

#ifndef COOPA_NODEGRAPH_GRAPH_H
#define COOPA_NODEGRAPH_GRAPH_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/nodegraph/declaration.h>
#include <coopa/nodegraph/diagnostics.h>
#include <coopa/nodegraph/node_type.h>
#include <coopa/nodegraph/type_registry.h>
#include <coopa/nodegraph/value.h>

namespace coopa {
namespace nodegraph {

/// @brief A node's identity within one Graph. 0 means "no node".
using NodeId = uint32_t;

/// @brief The node type name a Group node carries. Its sub-graph is named by `group_ref`.
inline constexpr const char* k_group_node_type = "Group";
/// @brief The node type that reads the graph's own interface inputs.
inline constexpr const char* k_group_input_type = "GroupInput";
/// @brief The node type that writes the graph's own interface outputs.
inline constexpr const char* k_group_output_type = "GroupOutput";

/**
 * @struct SocketRef
 * @brief One endpoint of a link: a node, and a socket identifier on it.
 */
struct SocketRef {
    NodeId      node = 0;
    std::string socket;

    bool operator==(const SocketRef& o) const { return node == o.node && socket == o.socket; }
};

/**
 * @struct Link
 * @brief A connection from one node's output to another's input.
 *
 * `order` only matters when `to` is a multi-input: it fixes the sequence the consuming node reads
 * its links in, and it survives edits so inserting a link does not shuffle the rest.
 */
struct Link {
    SocketRef from;
    SocketRef to;
    int       order = 0;
};

/**
 * @struct Node
 * @brief One authored node.
 */
struct Node {
    NodeId      id = 0;
    std::string type;        ///< A NodeTypeRegistry name, or k_group_node_type.
    std::string group_ref;   ///< For a Group node: the referenced graph's name.
    PropertyBag props;
    glm::vec2   ui_position{0.0f, 0.0f};   ///< Authoring only. The evaluator never reads it.
};

/**
 * @struct InterfaceSocket
 * @brief One of the graph's own named parameters or results.
 *
 * `min_value` / `max_value` are optional and exist so a UI can render a slider with the right
 * range without the consumer writing a schema per graph.
 */
struct InterfaceSocket {
    std::string identifier;
    std::string label;
    std::string tooltip;
    TypeId      type = k_invalid_type;
    Value       default_value;
    Value       min_value;
    Value       max_value;

    const std::string& display() const { return label.empty() ? identifier : label; }
};

/**
 * @class Graph
 * @brief The authored graph. Mutable, single-owner, not thread-safe.
 */
class Graph {
public:
    Graph() = default;
    explicit Graph(std::string name) : name_(std::move(name)) {}

    const std::string& name() const { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    // --- Nodes ---

    /// @brief Appends a node and returns its new id.
    NodeId add_node(std::string type, PropertyBag props = PropertyBag()) {
        Node n;
        n.id    = next_id_++;
        n.type  = std::move(type);
        n.props = std::move(props);
        nodes_.push_back(std::move(n));
        return nodes_.back().id;
    }

    /// @brief Appends a Group node referencing the graph named `ref`.
    NodeId add_group_node(std::string ref, PropertyBag props = PropertyBag()) {
        NodeId id = add_node(k_group_node_type, std::move(props));
        node(id)->group_ref = std::move(ref);
        return id;
    }

    /// @brief Removes a node and every link touching it.
    void remove_node(NodeId id) {
        links_.erase(std::remove_if(links_.begin(), links_.end(),
                                    [id](const Link& l) { return l.from.node == id || l.to.node == id; }),
                     links_.end());
        nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                                    [id](const Node& n) { return n.id == id; }),
                     nodes_.end());
    }

    Node* node(NodeId id) {
        for (auto& n : nodes_) {
            if (n.id == id) return &n;
        }
        return nullptr;
    }

    const Node* node(NodeId id) const {
        for (const auto& n : nodes_) {
            if (n.id == id) return &n;
        }
        return nullptr;
    }

    const std::vector<Node>& nodes() const { return nodes_; }
    std::vector<Node>&       nodes() { return nodes_; }

    /**
     * @brief Appends a node that already has an id, for a loader reading a saved graph.
     *
     * Keeps `next_id_` above every id present, so a node added after a load cannot collide with
     * one the file named. Returns false when `id` is 0 or already taken.
     */
    bool add_node_with_id(Node n) {
        if (n.id == 0 || node(n.id) != nullptr) return false;
        if (n.id >= next_id_) next_id_ = n.id + 1;
        nodes_.push_back(std::move(n));
        return true;
    }

    /// @brief The id `add_node()` will hand out next. For a saver that wants to pin it.
    NodeId next_id() const { return next_id_; }

    /// @brief The first node whose type is `type`, or 0. For finding GroupOutput.
    NodeId find_node_of_type(std::string_view type) const {
        for (const auto& n : nodes_) {
            if (n.type == type) return n.id;
        }
        return 0;
    }

    // --- Links ---

    /**
     * @brief Connects an output to an input.
     *
     * A non-multi input accepts one link: connecting a second **replaces** the first, which is
     * what a UI expects when you drop a wire onto an occupied socket. A multi-input appends, with
     * `order` set past every existing link on that socket.
     *
     * @return False when `from`/`to` name the same node, or either node does not exist.
     */
    bool add_link(SocketRef from, SocketRef to, bool multi_input_target = false) {
        if (from.node == to.node) return false;
        if (node(from.node) == nullptr || node(to.node) == nullptr) return false;
        if (multi_input_target) {
            int next = 0;
            for (const auto& l : links_) {
                if (l.to == to && l.order >= next) next = l.order + 1;
            }
            links_.push_back(Link{std::move(from), std::move(to), next});
        } else {
            links_.erase(std::remove_if(links_.begin(), links_.end(),
                                        [&to](const Link& l) { return l.to == to; }),
                         links_.end());
            links_.push_back(Link{std::move(from), std::move(to), 0});
        }
        return true;
    }

    /// @brief Removes every link into `to`.
    void remove_links_into(const SocketRef& to) {
        links_.erase(std::remove_if(links_.begin(), links_.end(),
                                    [&to](const Link& l) { return l.to == to; }),
                     links_.end());
    }

    const std::vector<Link>& links() const { return links_; }
    std::vector<Link>&       links() { return links_; }

    /**
     * @brief Removes exactly one link.
     *
     * `remove_links_into()` removes every link into a socket, which is the wrong answer for a
     * multi-input where a user picked one wire to cut. Matched by both endpoints and `order`, so
     * two links differing only in order are distinguishable.
     *
     * @return Whether a matching link was found.
     */
    bool remove_link(const Link& link) {
        for (auto it = links_.begin(); it != links_.end(); ++it) {
            if (it->from == link.from && it->to == link.to && it->order == link.order) {
                links_.erase(it);
                return true;
            }
        }
        return false;
    }

    /// @brief Every link feeding `to`, ordered by `order`.
    std::vector<const Link*> links_into(const SocketRef& to) const {
        std::vector<const Link*> out;
        for (const auto& l : links_) {
            if (l.to == to) out.push_back(&l);
        }
        for (size_t i = 1; i < out.size(); ++i) {
            for (size_t j = i; j > 0 && out[j]->order < out[j - 1]->order; --j) std::swap(out[j], out[j - 1]);
        }
        return out;
    }

    /**
     * @brief Every link leaving `from`, in declaration order.
     *
     * The mirror of `links_into()`. An editor needs it constantly -- to re-route wires when a node
     * moves, to grey an output that feeds nothing, to warn before deleting a node -- and without
     * it every caller re-scans `links()` by hand.
     */
    std::vector<const Link*> links_from(const SocketRef& from) const {
        std::vector<const Link*> out;
        for (const auto& l : links_) {
            if (l.from == from) out.push_back(&l);
        }
        return out;
    }

    /// @brief Every link touching `node`, either end. For a deletion preview or a node drag.
    std::vector<const Link*> links_of(NodeId node_id) const {
        std::vector<const Link*> out;
        for (const auto& l : links_) {
            if (l.from.node == node_id || l.to.node == node_id) out.push_back(&l);
        }
        return out;
    }

    /**
     * @brief Copies `ids` with fresh node ids, offset by `offset`.
     *
     * A link is copied only when **both** of its ends are in the set: a duplicated subgraph keeps
     * its internal wiring, and its connections to the nodes it was copied away from are dropped
     * rather than silently doubled onto the originals' upstream.
     *
     * @return The new ids, in the same order as `ids`. Ids that name no node are skipped.
     */
    std::vector<NodeId> duplicate_nodes(const std::vector<NodeId>& ids, glm::vec2 offset) {
        std::unordered_map<NodeId, NodeId> remap;
        std::vector<NodeId> made;
        made.reserve(ids.size());
        for (NodeId id : ids) {
            const Node* src = node(id);
            if (src == nullptr || remap.count(id) != 0) continue;
            Node copy = *src;             // props, group_ref and ui_position come along.
            copy.id = next_id_++;
            copy.ui_position += offset;
            remap.emplace(id, copy.id);
            made.push_back(copy.id);
            nodes_.push_back(std::move(copy));
        }
        // Collected first: push_back below would invalidate an iterator over links_.
        std::vector<Link> fresh;
        for (const auto& l : links_) {
            auto from_it = remap.find(l.from.node);
            auto to_it   = remap.find(l.to.node);
            if (from_it == remap.end() || to_it == remap.end()) continue;
            fresh.push_back(Link{SocketRef{from_it->second, l.from.socket},
                                 SocketRef{to_it->second, l.to.socket}, l.order});
        }
        for (auto& l : fresh) links_.push_back(std::move(l));
        return made;
    }

    // --- Interface ---

    std::vector<InterfaceSocket>&       inputs() { return inputs_; }
    const std::vector<InterfaceSocket>& inputs() const { return inputs_; }
    std::vector<InterfaceSocket>&       outputs() { return outputs_; }
    const std::vector<InterfaceSocket>& outputs() const { return outputs_; }

    /// @brief Index of the interface input named `id`, or -1.
    int find_input(std::string_view id) const {
        for (size_t i = 0; i < inputs_.size(); ++i) {
            if (inputs_[i].identifier == id) return static_cast<int>(i);
        }
        return -1;
    }

    /// @brief Index of the interface output named `id`, or -1.
    int find_output(std::string_view id) const {
        for (size_t i = 0; i < outputs_.size(); ++i) {
            if (outputs_[i].identifier == id) return static_cast<int>(i);
        }
        return -1;
    }

    /**
     * @brief The declaration a GroupInput / GroupOutput node carries, built from the interface.
     *
     * Those two node types are not in the NodeTypeRegistry -- their sockets ARE the interface, so
     * there is nothing static to register. Anything that needs to know what sockets they have
     * (validate(), compile(), and an editor laying them out or deciding whether a wire may land
     * on one) must come through here, or it will see a node with no sockets at all.
     *
     * @param as_outputs True for GroupInput (the graph's inputs are its outputs), false for
     *        GroupOutput (the graph's outputs are its inputs).
     */
    NodeDeclaration interface_declaration(bool as_outputs) const {
        NodeDeclaration decl;
        const std::vector<InterfaceSocket>& from = as_outputs ? inputs_ : outputs_;
        std::vector<SocketDecl>& into = as_outputs ? decl.outputs : decl.inputs;
        for (const auto& is : from) {
            SocketDecl sd;
            sd.identifier    = is.identifier;
            sd.label         = is.label;
            sd.type          = is.type;
            sd.default_value = is.default_value;
            into.push_back(std::move(sd));
        }
        return decl;
    }

    /**
     * @brief Checks everything that can be checked without flattening groups.
     *
     * Covers duplicate node ids, unknown node types, links to sockets that the node's declaration
     * does not have, type mismatches with no registered conversion, and cycles **within this
     * graph**. A cycle created by a group reference is caught during compilation instead, where
     * the reference chain is known.
     *
     * Never throws; fills `out` and returns whether it stayed error-free.
     */
    bool validate(Diagnostics& out) const {
        std::unordered_set<NodeId> seen_ids;
        std::unordered_map<NodeId, NodeDeclaration> decls;

        for (const auto& n : nodes_) {
            if (n.id == 0) {
                out.add(Severity::Error, 0, n.type, "node id 0 is reserved");
            }
            if (!seen_ids.insert(n.id).second) {
                out.add(Severity::Error, n.id, n.type, "duplicate node id");
            }
            if (n.type == k_group_node_type) {
                if (n.group_ref.empty()) {
                    out.add(Severity::Error, n.id, n.type, "Group node has no group_ref");
                }
                continue;   // Its sockets are only knowable once the reference resolves.
            }
            // GroupInput and GroupOutput are structural: they are not registered node types, and
            // their sockets come from this graph's own interface. Synthesizing their declarations
            // here rather than skipping them is what lets validate() check links against the
            // interface -- and it is what keeps validate() and compile() agreeing on what a valid
            // graph is, which they must, or a graph compiles but will not validate.
            if (n.type == k_group_input_type) {
                decls.emplace(n.id, interface_declaration(/*as_outputs=*/true));
                continue;
            }
            if (n.type == k_group_output_type) {
                decls.emplace(n.id, interface_declaration(/*as_outputs=*/false));
                continue;
            }
            const NodeType* type = NodeTypeRegistry::instance().find(n.type);
            if (type == nullptr) {
                out.add(Severity::Error, n.id, n.type, "unknown node type '" + n.type + "'");
                continue;
            }
            decls.emplace(n.id, type->declaration_for(n.props));
        }

        // One message per over-subscribed socket, not one per offending link. Keyed by a
        // comparable pair rather than SocketRef, which has equality but no ordering.
        std::set<std::pair<NodeId, std::string>> counted_doubles;
        for (const auto& l : links_) {
            const Node* from_node = node(l.from.node);
            const Node* to_node   = node(l.to.node);
            if (from_node == nullptr || to_node == nullptr) {
                out.error("link references a node that does not exist");
                continue;
            }
            // A Group endpoint is unverifiable here, by the same reasoning as above.
            if (from_node->type == k_group_node_type || to_node->type == k_group_node_type) continue;

            auto from_it = decls.find(l.from.node);
            auto to_it   = decls.find(l.to.node);
            if (from_it == decls.end() || to_it == decls.end()) continue;

            int oi = from_it->second.find_output(l.from.socket);
            int ii = to_it->second.find_input(l.to.socket);
            if (oi < 0) {
                out.add(Severity::Error, l.from.node, from_node->type,
                        "no output named '" + l.from.socket + "'");
                continue;
            }
            if (ii < 0) {
                out.add(Severity::Error, l.to.node, to_node->type,
                        "no input named '" + l.to.socket + "'");
                continue;
            }
            TypeId ft = from_it->second.outputs[oi].type;
            TypeId tt = to_it->second.inputs[ii].type;
            if (ft != tt && !TypeRegistry::instance().can_convert(ft, tt)) {
                const auto& reg = TypeRegistry::instance();
                out.add(Severity::Error, l.to.node, to_node->type,
                        "cannot connect " + reg.name_of(ft) + " to " + reg.name_of(tt) +
                        " (no conversion registered)");
            }

            // A plain input takes ONE link. add_link() enforces that by replacing, but a file
            // loaded through graph_from_node() pushes straight onto links_ and bypasses it -- so
            // a hand-written graph can state two, and the compiler would silently use the first
            // and discard the rest. Counted here rather than in the loader because it is the
            // graph's invariant, not the file format's.
            const std::pair<NodeId, std::string> socket_key{l.to.node, l.to.socket};
            if (!to_it->second.inputs[ii].multi_input() && !counted_doubles.count(socket_key)) {
                size_t n = 0;
                for (const auto& other : links_) {
                    if (other.to == l.to) ++n;
                }
                if (n > 1) {
                    counted_doubles.insert(socket_key);
                    out.add(Severity::Error, l.to.node, to_node->type,
                            "input '" + l.to.socket + "' is not a multi-input but has " +
                            std::to_string(n) + " links; only one would be used");
                }
            }
        }

        // An interface socket nothing is wired to is DEAD: the graph declares a parameter, a
        // consumer binds it, and changing it does nothing at all. Nothing else reports this --
        // the bake succeeds, the value is simply never read -- so it is a warning here, where
        // both the editor and a headless compile will see it.
        for (const InterfaceSocket& is : inputs_) {
            bool read = false;
            for (const Node& n : nodes_) {
                if (n.type != k_group_input_type) continue;
                for (const Link& l : links_) {
                    if (l.from.node == n.id && l.from.socket == is.identifier) read = true;
                }
            }
            if (!read) {
                out.add(Severity::Warning, 0, k_group_input_type,
                        "input '" + is.identifier + "' is declared but no node reads it, so "
                        "changing it has no effect");
            }
        }
        for (const InterfaceSocket& is : outputs_) {
            bool written = false;
            for (const Node& n : nodes_) {
                if (n.type != k_group_output_type) continue;
                for (const Link& l : links_) {
                    if (l.to.node == n.id && l.to.socket == is.identifier) written = true;
                }
            }
            if (!written) {
                out.add(Severity::Warning, 0, k_group_output_type,
                        "output '" + is.identifier + "' is declared but nothing is wired to it");
            }
        }

        detect_cycles_(out);
        return out.ok();
    }

private:
    /// @brief Kahn's algorithm; whatever is left when no node has in-degree 0 is in a cycle.
    void detect_cycles_(Diagnostics& out) const {
        std::unordered_map<NodeId, int> in_degree;
        std::unordered_map<NodeId, std::vector<NodeId>> consumers;
        for (const auto& n : nodes_) in_degree[n.id] = 0;
        for (const auto& l : links_) {
            if (in_degree.count(l.to.node) == 0 || in_degree.count(l.from.node) == 0) continue;
            ++in_degree[l.to.node];
            consumers[l.from.node].push_back(l.to.node);
        }
        std::vector<NodeId> ready;
        for (const auto& [id, deg] : in_degree) {
            if (deg == 0) ready.push_back(id);
        }
        size_t visited = 0;
        while (!ready.empty()) {
            NodeId id = ready.back();
            ready.pop_back();
            ++visited;
            auto it = consumers.find(id);
            if (it == consumers.end()) continue;
            for (NodeId c : it->second) {
                if (--in_degree[c] == 0) ready.push_back(c);
            }
        }
        if (visited == in_degree.size()) return;

        std::string ids;
        for (const auto& [id, deg] : in_degree) {
            if (deg > 0) {
                if (!ids.empty()) ids += ", ";
                ids += "#" + std::to_string(id);
            }
        }
        out.error("the graph has a cycle through these nodes: " + ids);
    }

    std::string                  name_;
    std::vector<Node>            nodes_;
    std::vector<Link>            links_;
    std::vector<InterfaceSocket> inputs_;
    std::vector<InterfaceSocket> outputs_;
    NodeId                       next_id_ = 1;   // 0 is reserved.
};

} // namespace nodegraph
} // namespace coopa

#endif // COOPA_NODEGRAPH_GRAPH_H
