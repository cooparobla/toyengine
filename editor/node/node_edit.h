/**
 * @file node_edit.h
 * @brief Graph mutations, as pure functions over a `Graph&`.
 *
 * Every edit the canvas can make lives here rather than inline in the panel, for the reason
 * `editor/ui/curve_math.h` gives: the behaviour is then unit-testable without a window, and the
 * widget is left turning clicks into calls.
 *
 * The rules that are easy to get wrong and are therefore enforced here rather than at each call
 * site: a connection is legal only if `TypeRegistry::can_convert()` says so (**the same predicate
 * `Graph::validate()` uses**, so a wire the canvas lets you drop can never fail validation); a
 * plain input replaces while a multi-input appends; and a link must run output-to-input, which
 * `Graph::add_link` does not itself check.
 */

#ifndef TOYENGINE_EDITOR_NODE_NODE_EDIT_H
#define TOYENGINE_EDITOR_NODE_NODE_EDIT_H

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/nodegraph/graph.h>

namespace toy {
namespace editor {
namespace node {

namespace ng = coopa::nodegraph;

/**
 * @struct NodeGraphEdit
 * @brief What an edit did, in the editor's usual shape.
 *
 * Mirrors `editor/schema/inspector.h`'s `EditResult` and `editor/viewport/gizmo.h`'s `GizmoDelta`:
 * `active` while a drag continues so undo steps merge, `finished` on the frame it ends so the step
 * closes, and `key` as the merge key and undo label.
 */
struct NodeGraphEdit {
    bool        changed = false;
    bool        active = false;
    bool        finished = false;
    std::string key;

    explicit operator bool() const { return changed; }
};

/// @brief Why a proposed connection was refused. `Ok` means it would be made.
enum class ConnectVerdict {
    Ok,
    SameNode,        ///< A node cannot feed itself.
    NotOppositeEnds, ///< Both ends are inputs, or both outputs.
    MissingSocket,   ///< One end names a socket its node's declaration does not have.
    TypeMismatch,    ///< No direct conversion is registered between the two socket types.
    WouldCycle,      ///< The link would close a loop.
};

/// @brief A one-line reason for a verdict, for a tooltip while a wire is being dragged.
const char* connect_verdict_text(ConnectVerdict v);

/**
 * @brief Whether `from` could feed `to`, and why not when it could not.
 *
 * Pure: makes no change. The canvas calls this every frame while a wire is dragged, to colour the
 * wire and highlight legal drop targets.
 */
ConnectVerdict can_connect(const ng::Graph& graph, const ng::SocketRef& from, const ng::SocketRef& to);

/**
 * @brief Connects `from` to `to` if `can_connect()` allows it.
 *
 * Handles the multi-input decision itself by reading the target's declaration, which
 * `Graph::add_link` leaves to its caller.
 */
NodeGraphEdit connect(ng::Graph& graph, const ng::SocketRef& from, const ng::SocketRef& to);

/// @brief Removes link `index` of `graph.links()`.
NodeGraphEdit disconnect(ng::Graph& graph, int index);

/// @brief Adds a node of `type` with its top-left at `at`.
NodeGraphEdit add_node_at(ng::Graph& graph, const std::string& type, glm::vec2 at, ng::NodeId& out_id);

/// @brief Removes every node in `ids`, and every link touching them.
NodeGraphEdit delete_nodes(ng::Graph& graph, const std::vector<ng::NodeId>& ids);

/// @brief Duplicates `ids`, offset so the copies are visibly not on top of the originals.
NodeGraphEdit duplicate(ng::Graph& graph, const std::vector<ng::NodeId>& ids,
                        std::vector<ng::NodeId>& out_new, glm::vec2 offset = {30.0f, 30.0f});

/// @brief Moves every node in `ids` by a graph-space delta. `finished` closes the undo step.
NodeGraphEdit move_nodes(ng::Graph& graph, const std::vector<ng::NodeId>& ids, glm::vec2 delta,
                         bool finished);

// --- The graph's own interface ---------------------------------------------------------------
//
// A GroupInput node's output sockets, and a GroupOutput node's input sockets, ARE the graph's
// interface -- `Graph::interface_declaration_()` synthesises them by name. So every one of these
// has to keep the links on those two structural nodes in step: an identifier is the only thing a
// link stores, and nothing else in the library will notice when one stops resolving.

/**
 * @brief Appends a socket to the graph's inputs (or outputs), with a unique identifier.
 * @param as_outputs Edit `outputs()` rather than `inputs()`.
 * @param type       Its socket type; its default value is that type's default.
 * @param id         Preferred identifier. Suffixed (`size_2`) when already taken; a blank one
 *                   becomes `input`/`output`.
 */
NodeGraphEdit add_interface_socket(ng::Graph& graph, bool as_outputs, ng::TypeId type,
                                   const std::string& id = {});

/// @brief Removes interface socket `index`, and every link on the structural node that fed it.
NodeGraphEdit remove_interface_socket(ng::Graph& graph, bool as_outputs, std::size_t index);

/**
 * @brief Renames interface socket `index`, re-pointing the links that name it.
 *
 * Refuses a blank identifier and one already in use, since either silently orphans every link
 * through that socket. Unchanged names are a no-op, not a failure.
 */
NodeGraphEdit rename_interface_socket(ng::Graph& graph, bool as_outputs, std::size_t index,
                                      const std::string& new_id);

/**
 * @brief Re-types interface socket `index`, resetting its default/min/max to the new type.
 *
 * Drops the links through it that the new type cannot feed -- `TypeRegistry::can_convert()`
 * decides, so the graph is left in a state `validate()` accepts.
 */
NodeGraphEdit retype_interface_socket(ng::Graph& graph, bool as_outputs, std::size_t index,
                                      ng::TypeId type);

/// @brief Moves interface socket `index` by `delta` places. Links travel with it, being by name.
NodeGraphEdit move_interface_socket(ng::Graph& graph, bool as_outputs, std::size_t index, int delta);

} // namespace node
} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_NODE_NODE_EDIT_H
