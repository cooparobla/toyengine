/**
 * @file node_layout.cpp
 * @brief The node geometry both the renderer and the hit-tester read.
 */

#include "editor/node/node_layout.h"

#include <algorithm>

#include <coopa/nodegraph/node_type.h>

namespace toy {
namespace editor {
namespace node {
namespace {

/// @brief Whether a declared socket occupies a row. Hidden sockets are authored-only.
bool visible_(const ng::SocketDecl& s) {
    return !ng::has_flag(s.flags, ng::SocketFlags::Hide);
}

} // namespace

NodeLayout layout_node(const ng::Node& node, const ng::NodeDeclaration& decl,
                       const std::string& title, const TextMeasure& measure,
                       const LayoutMetrics& m) {
    NodeLayout out;
    out.id = node.id;

    std::vector<const ng::SocketDecl*> ins, outs;
    for (const auto& s : decl.inputs) {
        if (visible_(s)) ins.push_back(&s);
    }
    for (const auto& s : decl.outputs) {
        if (visible_(s)) outs.push_back(&s);
    }

    // Inputs and outputs share rows rather than stacking, so a node is as tall as its busier side.
    const size_t rows = std::max(ins.size(), outs.size());
    const float height = m.header_h + m.pad_y * 2.0f + static_cast<float>(rows) * m.row_h;

    float width = m.min_width;
    if (measure) {
        float widest = measure(title);
        for (size_t i = 0; i < rows; ++i) {
            const float l = i < ins.size() ? measure(ins[i]->display()) : 0.0f;
            const float r = i < outs.size() ? measure(outs[i]->display()) : 0.0f;
            // Both labels share one row, so it is their SUM that has to fit, not the wider one.
            widest = std::max(widest, l + r + m.label_pad);
        }
        width = std::clamp(widest + m.label_pad, m.min_width, m.max_width);
    }

    out.lo = node.ui_position;
    out.hi = node.ui_position + glm::vec2(width, height);

    const float first_row = out.lo.y + m.header_h + m.pad_y + m.row_h * 0.5f;
    out.sockets.reserve(ins.size() + outs.size());
    for (size_t i = 0; i < ins.size(); ++i) {
        SocketLayout s;
        s.identifier  = ins[i]->identifier;
        s.is_input    = true;
        s.type        = ins[i]->type;
        s.multi_input = ins[i]->multi_input();
        s.row_y       = first_row + static_cast<float>(i) * m.row_h;
        s.pos         = {out.lo.x, s.row_y};   // On the edge: a wire ends where the body starts.
        out.sockets.push_back(std::move(s));
    }
    for (size_t i = 0; i < outs.size(); ++i) {
        SocketLayout s;
        s.identifier  = outs[i]->identifier;
        s.is_input    = false;
        s.type        = outs[i]->type;
        s.multi_input = false;
        s.row_y       = first_row + static_cast<float>(i) * m.row_h;
        s.pos         = {out.hi.x, s.row_y};
        out.sockets.push_back(std::move(s));
    }
    return out;
}

std::vector<NodeLayout> layout_graph(const ng::Graph& graph, const TextMeasure& measure,
                                     const LayoutMetrics& m) {
    std::vector<NodeLayout> out;
    out.reserve(graph.nodes().size());
    for (const auto& n : graph.nodes()) {
        // GroupInput and GroupOutput are STRUCTURAL: they are not registered node types, and
        // their sockets come from the graph's own interface. Synthesizing them here is what
        // makes the graph's inputs and outputs wirable at all -- without it they draw as bare
        // headers with nothing to connect to, which is a canvas you cannot finish a graph in.
        // Graph::validate() does exactly the same thing, for the same reason.
        if (n.type == ng::k_group_input_type || n.type == ng::k_group_output_type) {
            const bool as_outputs = n.type == ng::k_group_input_type;
            out.push_back(layout_node(n, interface_declaration(graph, as_outputs), n.type, measure, m));
            continue;
        }
        const ng::NodeType* type = ng::NodeTypeRegistry::instance().find(n.type);
        // An unregistered type lays out as a bare header. Dropping it would make a graph written
        // against a newer build look empty rather than merely unfamiliar.
        const ng::NodeDeclaration decl = type ? type->declaration_for(n.props) : ng::NodeDeclaration{};
        const std::string title = type ? type->display() : n.type;
        out.push_back(layout_node(n, decl, title, measure, m));
    }
    return out;
}

bool graph_bounds(const std::vector<NodeLayout>& layouts, glm::vec2& out_lo, glm::vec2& out_hi) {
    if (layouts.empty()) return false;
    out_lo = layouts.front().lo;
    out_hi = layouts.front().hi;
    for (const auto& l : layouts) {
        out_lo = glm::min(out_lo, l.lo);
        out_hi = glm::max(out_hi, l.hi);
    }
    return true;
}

} // namespace node
} // namespace editor
} // namespace toy
