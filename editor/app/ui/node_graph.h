/**
 * @file node_graph.h
 * @brief The node-graph workspace: a canvas for any `coopa::nodegraph::Graph`.
 *
 * The viewport switches to this when a graph asset is open, the way the UI designer replaces it
 * for a panel. Nothing here is specific to procedural geometry: node types, their categories,
 * socket types and the conversions between them all come from the library's registries, so a
 * second kind of graph needs no code in this file.
 *
 * Geometry and graph edits live in [`editor/node/`](../../node/README.md) as pure functions, so
 * this file is input handling, drawing and undo -- the `curve_math` / `curve_widget` split.
 *
 * ### Four things `imm` makes easy to get wrong here
 *
 * - The canvas is a plain `Box` + `push_clip`, **never** `begin_region(scroll = true)`: a scrolling
 *   region eats the wheel and zoom silently stops working.
 * - Node drags use raw `is_hovered()` + an owned drag struct, **not** `invisible_button`: once that
 *   claims `active_id_`, every other `is_hovered()` returns false, so a drop-target socket could
 *   not highlight while a wire is being dragged.
 * - Picks are resolved by one `hit_test()` per frame in the order socket > node > wire, because
 *   `imm` keeps no hit-test tree.
 * - Widget ids are keyed by `NodeId`, never by a node pointer -- `get_id(const void*)` hashes the
 *   address and `Graph` stores its nodes in a vector that reallocates.
 */

#ifndef TOYENGINE_EDITOR_APP_UI_NODE_GRAPH_H
#define TOYENGINE_EDITOR_APP_UI_NODE_GRAPH_H

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/nodegraph/diagnostics.h>
#include <coopa/nodegraph/graph.h>

#include "editor/app/editor_panel.h"
#include "editor/node/node_edit.h"
#include "editor/node/node_hit.h"
#include "editor/node/node_layout.h"
#include "editor/node/node_view.h"

namespace toy {
namespace editor {

namespace ng = coopa::nodegraph;

/**
 * @class NodeGraphPanel
 * @brief Edits one node graph asset.
 */
class NodeGraphPanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    // --- the document ---

    /** @brief Opens a graph asset. False (and logged) if it will not parse. */
    bool open_graph(const std::filesystem::path& path);
    /** @brief Creates `name` from a blank template and opens it. */
    bool new_graph(const std::string& name);
    /** @brief Writes the open graph back to its file. */
    void save();
    /** @brief Whether the open graph has unsaved edits. */
    bool dirty() const;
    /** @brief True while a graph is open. */
    bool open() const;
    /** @brief The open graph's path, or empty. */
    std::filesystem::path path() const;

    /** @brief Re-reads the document into the live graph. After an undo, a redo or a reload. */
    void refresh_from_document();

    // --- drawing, called from EditorApp ---

    /** @brief The workspace's viewport: header, grid, wires, nodes. */
    void draw_graph_view_(imm::Context& ctx, const imm::Box& area);
    /** @brief The left column: the node palette, searchable, grouped by category. */
    void draw_graph_palette_(imm::Context& ctx, const imm::Box& area);
    /** @brief Properties of the graph itself: its name and its interface. */
    void draw_graph_props_(imm::Context& ctx);
    /** @brief Properties of the selected node: its settings and its unconnected input defaults. */
    void draw_node_props_(imm::Context& ctx);

    // --- view ---

    /** @brief Frames every node on the next draw. */
    void frame_all() { want_frame_all_ = true; }
    /** @brief Frames the selection, or everything when nothing is selected. */
    void frame_selection();

    /** @brief The selected nodes. */
    const std::vector<ng::NodeId>& selection() const { return selection_; }
    /** @brief The graph being edited. */
    const ng::Graph& graph() const { return graph_; }

    // --- operations, as the canvas and the keymap invoke them ---

    /** @brief Selects exactly `id`, or clears the selection when it is 0. */
    void select(ng::NodeId id) { select_only_(id); }
    /** @brief Moves the selection by a graph-space delta, as one undo step. */
    void move_selection(glm::vec2 delta);
    /** @brief Deletes the selection, links included. */
    void delete_selection();
    /** @brief Duplicates the selection and selects the copies. */
    void duplicate_selection();

private:
    // --- drawing helpers ---
    void draw_grid_(imm::Context& ctx, const imm::Box& area);
    void draw_wires_(imm::Context& ctx, const imm::Box& area);
    void draw_nodes_(imm::Context& ctx, const imm::Box& area);
    void draw_header_(imm::Context& ctx, const imm::Box& b);
    void draw_add_popup_(imm::Context& ctx);

    // --- input ---
    void handle_input_(imm::Context& ctx, const imm::Box& area);
    void commit_(const node::NodeGraphEdit& edit);
    /**
     * @struct InterfaceDrag
     * @brief A grip being dragged to reorder one interface socket.
     *
     * `anchor_y` is where the mouse was when the row last moved, not where the drag began, so a
     * drag across several rows steps once per row rather than swapping once and sticking.
     */
    struct InterfaceDrag {
        bool        active = false;
        bool        as_outputs = false;
        std::size_t index = 0;
        float       anchor_y = 0.0f;
    };
    InterfaceDrag iface_drag_;

    /// @brief The add/remove/rename/retype rows for the graph's inputs, or its outputs.
    void draw_interface_(imm::Context& ctx, bool as_outputs);

    /// @brief The live graph, its layouts and its diagnostics, after any edit.
    void relayout_(imm::Context& ctx);
    void revalidate_();

    /// @brief The colour for a socket of `type`, from the theme's per-type table.
    glm::vec4 socket_color_(ng::TypeId type) const;

    bool selected_(ng::NodeId id) const;
    void select_only_(ng::NodeId id);
    void toggle_select_(ng::NodeId id);

    /// @brief The graph the canvas edits. Mirrors the document; written back on every commit.
    ng::Graph graph_;
    /// @brief Recomputed whenever the graph or the text metrics change.
    std::vector<node::NodeLayout> layouts_;
    node::LayoutMetrics           metrics_;
    node::NodeView                view_;

    /// @brief From validate(), keyed by node id, for the per-node error badge.
    std::unordered_map<ng::NodeId, std::string> node_errors_;
    std::string                                 graph_error_;

    std::vector<ng::NodeId> selection_;
    bool                    want_frame_all_ = true;

    /// @brief A node drag. Raw rather than through invisible_button -- see the @file doc.
    struct NodeDrag {
        bool      active = false;
        bool      moved = false;
        glm::vec2 last{0.0f};   ///< Graph space, so the delta is zoom-independent.
    } drag_;

    /// @brief A wire being pulled from a socket.
    struct WireDrag {
        bool        active = false;
        ng::NodeId  node = 0;
        std::string socket;
        bool        from_input = false;   ///< Dragging backwards, from an input.
    } wire_;

    /// @brief A rubber band. The 5 px threshold is what separates a band from a click-pick.
    struct BandSelect {
        bool      pending = false;
        bool      active = false;
        glm::vec2 press{0.0f};   ///< Screen space: the threshold is in pixels, not graph units.
    } band_;

    bool      panning_ = false;
    glm::vec2 add_at_{0.0f};      ///< Graph-space point the Add popup will place its node at.
    std::string palette_filter_;
};

} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_APP_UI_NODE_GRAPH_H
