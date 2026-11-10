/**
 * @file graph_yaml.h
 * @brief Reading and writing a `Graph` as YAML.
 *
 * ### The file format
 *
 * @code{.yaml}
 * name: railing
 * inputs:                       # the graph's own parameters
 *   - id: length
 *     type: Float
 *     label: Length
 *     default: 4.0
 *     min: 0.1
 *     max: 100.0
 * outputs:
 *   - id: geometry
 *     type: Geometry
 * nodes:
 *   - id: 1
 *     type: GroupInput
 *     pos: [-200, 0]
 *   - id: 2
 *     type: Math
 *     pos: [0, 0]
 *     props: { operation: multiply }
 * links:
 *   - [1, length, 2, a]         # from_node, from_socket, to_node, to_socket
 *   - [1, length, 2, b, 1]      # ...with an explicit multi-input order
 * @endcode
 *
 * ### Round-tripping is a tested property, not an aspiration
 *
 * Everything goes through `coopa::yaml::load_document` / `coopa::yaml::emit` with an explicit key
 * order, so load -> save -> load is byte-identical. Two things make that work: a node's unknown
 * `props` keys are preserved verbatim by `PropertyBag` (so a graph authored against a newer build
 * survives a round trip through an older one), and node ids are written rather than reassigned.
 *
 * A socket default can only be written for a type that registered YAML hooks (see
 * `TypeRegistry::set_yaml_hooks`). A type without them still works -- its sockets simply carry no
 * authored default, and a warning says so rather than the value silently vanishing.
 */

#ifndef COOPA_NODEGRAPH_GRAPH_YAML_H
#define COOPA_NODEGRAPH_GRAPH_YAML_H

#include <string>
#include <utility>
#include <vector>

#include <fkYAML/node.hpp>

#include <coopa/nodegraph/diagnostics.h>
#include <coopa/nodegraph/graph.h>
#include <coopa/nodegraph/type_registry.h>
#include <coopa/nodegraph/value.h>
#include <coopa/yaml/document.h>
#include <coopa/yaml/writer.h>

namespace coopa {
namespace nodegraph {

namespace detail {

/// @brief Reads one interface socket's optional Value field through the type's YAML hook.
inline Value read_socket_value_(const fkyaml::node& owner, const char* key, TypeId type,
                                Diagnostics& diag, const std::string& socket_id) {
    if (!owner.is_mapping() || !owner.contains(key)) return Value();
    const TypeInfo& info = TypeRegistry::instance().info(type);
    if (info.from_yaml == nullptr) {
        diag.warn("socket '" + socket_id + "' has a '" + key + "' but type '" + info.name +
                  "' registered no YAML hook; ignoring it");
        return Value();
    }
    Value v = Value::make_default(type);
    if (v.empty()) return v;
    if (!info.from_yaml(owner.at(key), v.mutable_data())) {
        diag.warn("socket '" + socket_id + "' has a '" + key + "' that type '" + info.name +
                  "' could not parse");
        return Value();
    }
    return v;
}

/// @brief Writes a Value through its type's YAML hook. Returns false when it cannot be written.
inline bool write_socket_value_(const Value& v, fkyaml::node& out) {
    if (v.empty()) return false;
    const TypeInfo& info = TypeRegistry::instance().info(v.type());
    if (info.to_yaml == nullptr || v.data() == nullptr) return false;
    return info.to_yaml(v.data(), out);
}

inline void read_interface_(const fkyaml::node& list, std::vector<InterfaceSocket>& out,
                            Diagnostics& diag) {
    if (!list.is_sequence()) return;
    for (const auto& entry : list) {
        if (!entry.is_mapping() || !entry.contains("id")) {
            diag.warn("an interface socket has no 'id' and was skipped");
            continue;
        }
        InterfaceSocket s;
        s.identifier = entry.at("id").get_value<std::string>();
        if (entry.contains("label")) s.label = entry.at("label").get_value<std::string>();
        if (entry.contains("tooltip")) s.tooltip = entry.at("tooltip").get_value<std::string>();
        std::string type_name = entry.contains("type") ? entry.at("type").get_value<std::string>()
                                                       : std::string();
        s.type = TypeRegistry::instance().find(type_name);
        if (s.type == k_invalid_type) {
            diag.error("interface socket '" + s.identifier + "' has unregistered type '" +
                       type_name + "'");
            continue;
        }
        s.default_value = read_socket_value_(entry, "default", s.type, diag, s.identifier);
        s.min_value     = read_socket_value_(entry, "min", s.type, diag, s.identifier);
        s.max_value     = read_socket_value_(entry, "max", s.type, diag, s.identifier);
        out.push_back(std::move(s));
    }
}

inline fkyaml::node write_interface_(const std::vector<InterfaceSocket>& sockets) {
    fkyaml::node list = fkyaml::node::sequence();
    for (const auto& s : sockets) {
        fkyaml::node entry = fkyaml::node::mapping();
        entry["id"]   = fkyaml::node(s.identifier);
        entry["type"] = fkyaml::node(TypeRegistry::instance().name_of(s.type));
        if (!s.label.empty()) entry["label"] = fkyaml::node(s.label);
        if (!s.tooltip.empty()) entry["tooltip"] = fkyaml::node(s.tooltip);
        fkyaml::node v;
        if (write_socket_value_(s.default_value, v)) entry["default"] = v;
        fkyaml::node mn;
        if (write_socket_value_(s.min_value, mn)) entry["min"] = mn;
        fkyaml::node mx;
        if (write_socket_value_(s.max_value, mx)) entry["max"] = mx;
        list.as_seq().push_back(std::move(entry));
    }
    return list;
}

} // namespace detail

/**
 * @brief Parses a graph from an already-loaded YAML node.
 * @return The graph. Check `diag.ok()` -- a graph with errors is returned partially built so a
 *         tool can still show what it managed to read.
 */
inline Graph graph_from_node(const fkyaml::node& root, Diagnostics& diag) {
    Graph graph;
    if (!root.is_mapping()) {
        diag.error("a graph file must be a mapping");
        return graph;
    }
    if (root.contains("name")) graph.set_name(root.at("name").get_value<std::string>());

    if (root.contains("inputs")) detail::read_interface_(root.at("inputs"), graph.inputs(), diag);
    if (root.contains("outputs")) detail::read_interface_(root.at("outputs"), graph.outputs(), diag);

    if (root.contains("nodes") && root.at("nodes").is_sequence()) {
        for (const auto& entry : root.at("nodes")) {
            if (!entry.is_mapping()) continue;
            Node n;
            if (!entry.contains("id") || !entry.contains("type")) {
                diag.error("a node entry needs both 'id' and 'type'");
                continue;
            }
            n.id   = static_cast<NodeId>(entry.at("id").get_value<int64_t>());
            n.type = entry.at("type").get_value<std::string>();
            if (entry.contains("group")) n.group_ref = entry.at("group").get_value<std::string>();
            if (entry.contains("props")) n.props = PropertyBag(entry.at("props"));
            if (entry.contains("pos") && entry.at("pos").is_sequence() && entry.at("pos").size() >= 2) {
                n.ui_position.x = static_cast<float>(entry.at("pos")[0].get_value<double>());
                n.ui_position.y = static_cast<float>(entry.at("pos")[1].get_value<double>());
            }
            if (!graph.add_node_with_id(std::move(n))) {
                diag.error("node id " + std::to_string(n.id) + " is zero or duplicated");
            }
        }
    }

    if (root.contains("links") && root.at("links").is_sequence()) {
        for (const auto& entry : root.at("links")) {
            // A link is a flow sequence so the file stays readable at a glance.
            if (!entry.is_sequence() || entry.size() < 4) {
                diag.error("a link must be [from_node, from_socket, to_node, to_socket[, order]]");
                continue;
            }
            Link l;
            l.from.node   = static_cast<NodeId>(entry[0].get_value<int64_t>());
            l.from.socket = entry[1].get_value<std::string>();
            l.to.node     = static_cast<NodeId>(entry[2].get_value<int64_t>());
            l.to.socket   = entry[3].get_value<std::string>();
            l.order       = entry.size() >= 5 ? static_cast<int>(entry[4].get_value<int64_t>()) : 0;
            if (graph.node(l.from.node) == nullptr || graph.node(l.to.node) == nullptr) {
                diag.error("a link references a node that is not in the file");
                continue;
            }
            graph.links().push_back(std::move(l));
        }
    }

    return graph;
}

/// @brief Serializes a graph to a YAML node.
inline fkyaml::node graph_to_node(const Graph& graph) {
    fkyaml::node root = fkyaml::node::mapping();
    if (!graph.name().empty()) root["name"] = fkyaml::node(graph.name());
    if (!graph.inputs().empty()) root["inputs"] = detail::write_interface_(graph.inputs());
    if (!graph.outputs().empty()) root["outputs"] = detail::write_interface_(graph.outputs());

    fkyaml::node nodes = fkyaml::node::sequence();
    for (const auto& n : graph.nodes()) {
        fkyaml::node entry = fkyaml::node::mapping();
        entry["id"]   = fkyaml::node(static_cast<int64_t>(n.id));
        entry["type"] = fkyaml::node(n.type);
        if (!n.group_ref.empty()) entry["group"] = fkyaml::node(n.group_ref);
        fkyaml::node pos = fkyaml::node::sequence();
        pos.as_seq().push_back(fkyaml::node(static_cast<double>(n.ui_position.x)));
        pos.as_seq().push_back(fkyaml::node(static_cast<double>(n.ui_position.y)));
        entry["pos"] = std::move(pos);
        if (!n.props.empty()) entry["props"] = n.props.raw();
        nodes.as_seq().push_back(std::move(entry));
    }
    root["nodes"] = std::move(nodes);

    fkyaml::node links = fkyaml::node::sequence();
    for (const auto& l : graph.links()) {
        fkyaml::node entry = fkyaml::node::sequence();
        entry.as_seq().push_back(fkyaml::node(static_cast<int64_t>(l.from.node)));
        entry.as_seq().push_back(fkyaml::node(l.from.socket));
        entry.as_seq().push_back(fkyaml::node(static_cast<int64_t>(l.to.node)));
        entry.as_seq().push_back(fkyaml::node(l.to.socket));
        if (l.order != 0) entry.as_seq().push_back(fkyaml::node(static_cast<int64_t>(l.order)));
        links.as_seq().push_back(std::move(entry));
    }
    root["links"] = std::move(links);
    return root;
}

/**
 * @brief The canonical key order, so a saved graph is stable rather than hash-ordered.
 *
 * `coopa::yaml::KeyOrder` ranks a key within its parent mapping: a lower number sorts earlier and
 * a negative one means "unranked", which falls back to alphabetical. Ranking per parent is what
 * lets a node entry lead with `id`/`type` while the root leads with `name`.
 */
inline const coopa::yaml::KeyOrder& graph_key_order() {
    static const coopa::yaml::KeyOrder order =
        [](const std::string& key, const std::string& parent) -> int {
        static const char* root_keys[]      = {"name", "inputs", "outputs", "nodes", "links"};
        static const char* node_keys[]      = {"id", "type", "group", "pos", "props"};
        static const char* interface_keys[] = {"id", "type", "label", "tooltip",
                                               "default", "min", "max"};
        auto rank_in = [&key](const char* const* table, size_t n) -> int {
            for (size_t i = 0; i < n; ++i) {
                if (key == table[i]) return static_cast<int>(i);
            }
            return -1;
        };
        if (parent == "nodes") return rank_in(node_keys, 5);
        if (parent == "inputs" || parent == "outputs") return rank_in(interface_keys, 7);
        if (parent.empty()) return rank_in(root_keys, 5);
        return -1;
    };
    return order;
}

/// @brief Loads a graph from a file, going through the shared document seam (so `.caml` works).
inline Graph load_graph(const std::string& path, Diagnostics& diag) {
    try {
        fkyaml::node root = coopa::yaml::load_document(path);
        Graph g = graph_from_node(root, diag);
        if (g.name().empty()) {
            // Fall back to the file stem, so an unnamed file still reports usefully.
            size_t slash = path.find_last_of("/\\");
            size_t dot   = path.find_last_of('.');
            size_t begin = slash == std::string::npos ? 0 : slash + 1;
            size_t count = dot == std::string::npos || dot < begin ? std::string::npos : dot - begin;
            g.set_name(path.substr(begin, count));
        }
        return g;
    } catch (const std::exception& e) {
        diag.error("could not read graph '" + path + "': " + e.what());
        return Graph();
    }
}

/// @brief Writes a graph to a file atomically, in canonical key order.
inline bool save_graph(const std::string& path, const Graph& graph, Diagnostics& diag) {
    try {
        coopa::yaml::save_document(path, graph_to_node(graph), graph_key_order());
        return true;
    } catch (const std::exception& e) {
        diag.error("could not write graph '" + path + "': " + e.what());
        return false;
    }
}

/// @brief A graph serialized to a YAML string, for a test or an in-memory round trip.
inline std::string graph_to_string(const Graph& graph) {
    return coopa::yaml::emit(graph_to_node(graph), graph_key_order());
}

} // namespace nodegraph
} // namespace coopa

#endif // COOPA_NODEGRAPH_GRAPH_YAML_H
