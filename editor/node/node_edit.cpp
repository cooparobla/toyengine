/**
 * @file node_edit.cpp
 * @brief The graph mutations, and the connection rules.
 */

#include "editor/node/node_edit.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <coopa/nodegraph/node_type.h>
#include <coopa/nodegraph/type_registry.h>

namespace toy {
namespace editor {
namespace node {
namespace {

/// @brief A node's current declaration, or an empty one for an unregistered type.
ng::NodeDeclaration decl_of_(const ng::Graph& graph, ng::NodeId id) {
    const ng::Node* n = graph.node(id);
    if (n == nullptr) return {};
    // GroupInput / GroupOutput are structural: they are not in the registry, and their sockets
    // come from the graph's own interface. Without this they would look socketless, and every
    // wire onto the graph's output would be refused as MissingSocket.
    if (n->type == ng::k_group_input_type)  return graph.interface_declaration(/*as_outputs=*/true);
    if (n->type == ng::k_group_output_type) return graph.interface_declaration(/*as_outputs=*/false);
    const ng::NodeType* t = ng::NodeTypeRegistry::instance().find(n->type);
    return t ? t->declaration_for(n->props) : ng::NodeDeclaration{};
}

/// @brief Whether `to` is reachable from `from` by following links. Used to refuse a cycle.
bool reaches_(const ng::Graph& graph, ng::NodeId from, ng::NodeId to) {
    std::unordered_set<ng::NodeId> seen;
    std::vector<ng::NodeId> stack{from};
    while (!stack.empty()) {
        const ng::NodeId at = stack.back();
        stack.pop_back();
        if (at == to) return true;
        if (!seen.insert(at).second) continue;
        for (const auto& l : graph.links()) {
            if (l.from.node == at) stack.push_back(l.to.node);
        }
    }
    return false;
}

} // namespace

const char* connect_verdict_text(ConnectVerdict v) {
    switch (v) {
        case ConnectVerdict::Ok:              return "Connect";
        case ConnectVerdict::SameNode:        return "A node cannot feed itself";
        case ConnectVerdict::NotOppositeEnds: return "Connect an output to an input";
        case ConnectVerdict::MissingSocket:   return "That socket no longer exists";
        case ConnectVerdict::TypeMismatch:    return "These types cannot be converted";
        case ConnectVerdict::WouldCycle:      return "That would make a loop";
    }
    return "";
}

ConnectVerdict can_connect(const ng::Graph& graph, const ng::SocketRef& from, const ng::SocketRef& to) {
    if (from.node == to.node) return ConnectVerdict::SameNode;

    const ng::NodeDeclaration from_decl = decl_of_(graph, from.node);
    const ng::NodeDeclaration to_decl   = decl_of_(graph, to.node);

    const int oi = from_decl.find_output(from.socket);
    const int ii = to_decl.find_input(to.socket);
    if (oi < 0 || ii < 0) {
        // Either the sockets are gone, or the user dragged output-to-output. Distinguishing the
        // two gives a message worth reading.
        const bool from_is_input  = from_decl.find_input(from.socket) >= 0;
        const bool to_is_output   = to_decl.find_output(to.socket) >= 0;
        if (from_is_input || to_is_output) return ConnectVerdict::NotOppositeEnds;
        return ConnectVerdict::MissingSocket;
    }

    const ng::TypeId ft = from_decl.outputs[oi].type;
    const ng::TypeId tt = to_decl.inputs[ii].type;
    // The same predicate validate() uses, so a wire the canvas accepts cannot later fail to
    // validate -- highlighting and validation are physically unable to disagree.
    if (ft != tt && !ng::TypeRegistry::instance().can_convert(ft, tt)) return ConnectVerdict::TypeMismatch;

    // The new link runs from -> to, so it closes a loop exactly when `to` already reaches `from`.
    if (reaches_(graph, to.node, from.node)) return ConnectVerdict::WouldCycle;
    return ConnectVerdict::Ok;
}

NodeGraphEdit connect(ng::Graph& graph, const ng::SocketRef& from, const ng::SocketRef& to) {
    NodeGraphEdit out;
    if (can_connect(graph, from, to) != ConnectVerdict::Ok) return out;

    const ng::NodeDeclaration to_decl = decl_of_(graph, to.node);
    const int ii = to_decl.find_input(to.socket);
    const bool multi = ii >= 0 && to_decl.inputs[ii].multi_input();

    // Graph::add_link leaves the multi-input decision to its caller; reading it off the
    // declaration here is what keeps every call site from having to remember.
    if (!graph.add_link(from, to, multi)) return out;
    out.changed  = true;
    out.finished = true;
    out.key      = "Connect";
    return out;
}

NodeGraphEdit disconnect(ng::Graph& graph, int index) {
    NodeGraphEdit out;
    if (index < 0 || static_cast<size_t>(index) >= graph.links().size()) return out;
    graph.links().erase(graph.links().begin() + index);
    out.changed  = true;
    out.finished = true;
    out.key      = "Disconnect";
    return out;
}

NodeGraphEdit add_node_at(ng::Graph& graph, const std::string& type, glm::vec2 at, ng::NodeId& out_id) {
    NodeGraphEdit out;
    out_id = graph.add_node(type);
    if (out_id == 0) return out;
    if (ng::Node* n = graph.node(out_id)) n->ui_position = at;
    out.changed  = true;
    out.finished = true;
    out.key      = "Add " + type;
    return out;
}

NodeGraphEdit delete_nodes(ng::Graph& graph, const std::vector<ng::NodeId>& ids) {
    NodeGraphEdit out;
    for (ng::NodeId id : ids) {
        if (graph.node(id) == nullptr) continue;
        graph.remove_node(id);   // Takes its links with it.
        out.changed = true;
    }
    if (!out.changed) return out;
    out.finished = true;
    out.key      = ids.size() == 1 ? "Delete Node" : "Delete Nodes";
    return out;
}

NodeGraphEdit duplicate(ng::Graph& graph, const std::vector<ng::NodeId>& ids,
                        std::vector<ng::NodeId>& out_new, glm::vec2 offset) {
    NodeGraphEdit out;
    out_new = graph.duplicate_nodes(ids, offset);
    if (out_new.empty()) return out;
    out.changed  = true;
    out.finished = true;
    out.key      = out_new.size() == 1 ? "Duplicate Node" : "Duplicate Nodes";
    return out;
}

NodeGraphEdit move_nodes(ng::Graph& graph, const std::vector<ng::NodeId>& ids, glm::vec2 delta,
                         bool finished) {
    NodeGraphEdit out;
    if (delta != glm::vec2(0.0f)) {
        for (ng::NodeId id : ids) {
            if (ng::Node* n = graph.node(id)) {
                n->ui_position += delta;
                out.changed = true;
            }
        }
    }
    // `active` while the drag runs keeps every frame's move merging into one undo step, which is
    // why the key is constant rather than naming the node.
    out.active   = !finished;
    out.finished = finished;
    out.key      = "Move Nodes";
    return out;
}


// --- The graph's own interface ---------------------------------------------------------------

namespace {

/// @brief The structural node whose sockets mirror this end of the interface, or 0.
ng::NodeId structural_node_(const ng::Graph& graph, bool as_outputs) {
    // An input of the graph is read by GroupInput; an output is written by GroupOutput.
    const char* want = as_outputs ? ng::k_group_output_type : ng::k_group_input_type;
    for (const ng::Node& n : graph.nodes()) if (n.type == want) return n.id;
    return 0;
}

/// @brief Whether `link`'s end on the structural node names `socket`.
bool touches_(const ng::Link& link, ng::NodeId node, const std::string& socket, bool as_outputs) {
    // GroupInput feeds the graph, so its sockets are link SOURCES; GroupOutput's are targets.
    const ng::SocketRef& end = as_outputs ? link.to : link.from;
    return end.node == node && end.socket == socket;
}

/// @brief An identifier not already used at this end of the interface.
std::string unique_id_(const std::vector<ng::InterfaceSocket>& sockets, std::string want,
                       const char* fallback) {
    if (want.empty()) want = fallback;
    auto taken = [&](const std::string& id) {
        return std::any_of(sockets.begin(), sockets.end(),
                           [&](const ng::InterfaceSocket& s) { return s.identifier == id; });
    };
    if (!taken(want)) return want;
    for (int n = 2; n < 10000; ++n) {
        std::string candidate = want + "_" + std::to_string(n);
        if (!taken(candidate)) return candidate;
    }
    return want;
}

} // namespace

NodeGraphEdit add_interface_socket(ng::Graph& graph, bool as_outputs, ng::TypeId type,
                                   const std::string& id) {
    NodeGraphEdit out;
    if (type == ng::k_invalid_type) return out;
    std::vector<ng::InterfaceSocket>& sockets = as_outputs ? graph.outputs() : graph.inputs();

    ng::InterfaceSocket s;
    s.identifier    = unique_id_(sockets, id, as_outputs ? "output" : "input");
    s.type          = type;
    s.default_value = ng::Value::make_default(type);
    sockets.push_back(std::move(s));

    out.changed  = true;
    out.finished = true;
    out.key      = as_outputs ? "Add Output" : "Add Input";
    return out;
}

NodeGraphEdit remove_interface_socket(ng::Graph& graph, bool as_outputs, std::size_t index) {
    NodeGraphEdit out;
    std::vector<ng::InterfaceSocket>& sockets = as_outputs ? graph.outputs() : graph.inputs();
    if (index >= sockets.size()) return out;

    const std::string gone = sockets[index].identifier;
    sockets.erase(sockets.begin() + static_cast<std::ptrdiff_t>(index));

    // The socket is gone from the structural node's declaration, so any link naming it would
    // dangle. Nothing downstream re-checks that, so drop them here.
    if (const ng::NodeId sn = structural_node_(graph, as_outputs)) {
        std::vector<ng::Link>& links = graph.links();
        links.erase(std::remove_if(links.begin(), links.end(),
                                   [&](const ng::Link& l) { return touches_(l, sn, gone, as_outputs); }),
                    links.end());
    }

    out.changed  = true;
    out.finished = true;
    out.key      = as_outputs ? "Remove Output" : "Remove Input";
    return out;
}

NodeGraphEdit rename_interface_socket(ng::Graph& graph, bool as_outputs, std::size_t index,
                                      const std::string& new_id) {
    NodeGraphEdit out;
    std::vector<ng::InterfaceSocket>& sockets = as_outputs ? graph.outputs() : graph.inputs();
    if (index >= sockets.size() || new_id.empty()) return out;
    const std::string old_id = sockets[index].identifier;
    if (new_id == old_id) return out;
    for (std::size_t i = 0; i < sockets.size(); ++i) {
        if (i != index && sockets[i].identifier == new_id) return out;   // Would orphan its links.
    }

    sockets[index].identifier = new_id;
    if (const ng::NodeId sn = structural_node_(graph, as_outputs)) {
        for (ng::Link& l : graph.links()) {
            ng::SocketRef& end = as_outputs ? l.to : l.from;
            if (end.node == sn && end.socket == old_id) end.socket = new_id;
        }
    }

    out.changed  = true;
    out.finished = true;
    out.key      = "Rename " + old_id;
    return out;
}

NodeGraphEdit retype_interface_socket(ng::Graph& graph, bool as_outputs, std::size_t index,
                                      ng::TypeId type) {
    NodeGraphEdit out;
    std::vector<ng::InterfaceSocket>& sockets = as_outputs ? graph.outputs() : graph.inputs();
    if (index >= sockets.size() || type == ng::k_invalid_type) return out;
    if (sockets[index].type == type) return out;

    sockets[index].type          = type;
    sockets[index].default_value = ng::Value::make_default(type);
    sockets[index].min_value     = ng::Value{};
    sockets[index].max_value     = ng::Value{};

    // Whatever the socket used to carry, its links were legal for the OLD type. Re-ask the same
    // question validate() will ask, and drop the ones that no longer answer yes.
    const ng::NodeId sn = structural_node_(graph, as_outputs);
    if (sn != 0) {
        const std::string& id = sockets[index].identifier;
        std::vector<ng::Link>& links = graph.links();
        links.erase(std::remove_if(links.begin(), links.end(), [&](const ng::Link& l) {
                        if (!touches_(l, sn, id, as_outputs)) return false;
                        return can_connect(graph, l.from, l.to) != ConnectVerdict::Ok;
                    }),
                    links.end());
    }

    out.changed  = true;
    out.finished = true;
    out.key      = "Change Type";
    return out;
}

NodeGraphEdit move_interface_socket(ng::Graph& graph, bool as_outputs, std::size_t index,
                                    int delta) {
    NodeGraphEdit out;
    std::vector<ng::InterfaceSocket>& sockets = as_outputs ? graph.outputs() : graph.inputs();
    if (index >= sockets.size() || delta == 0) return out;
    const long target = static_cast<long>(index) + delta;
    if (target < 0 || target >= static_cast<long>(sockets.size())) return out;

    // Links name sockets, never positions, so reordering needs no relinking -- it only changes
    // the row order the canvas and the component inspector draw.
    std::swap(sockets[index], sockets[static_cast<std::size_t>(target)]);

    out.changed  = true;
    out.finished = true;
    out.key      = "Reorder Interface";
    return out;
}

} // namespace node
} // namespace editor
} // namespace toy
