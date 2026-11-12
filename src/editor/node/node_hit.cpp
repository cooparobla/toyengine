/**
 * @file node_hit.cpp
 * @brief Picking, in the fixed priority socket > node > wire > background.
 */

#include "editor/node/node_hit.h"

#include <algorithm>
#include <cmath>

#include "editor/node/node_wire.h"
#include "editor/viewport/gizmo.h"

namespace toy {
namespace editor {
namespace node {
namespace {

const NodeLayout* layout_of_(const std::vector<NodeLayout>& layouts, ng::NodeId id) {
    for (const auto& l : layouts) {
        if (l.id == id) return &l;
    }
    return nullptr;
}

} // namespace

bool node_contains(const NodeLayout& layout, glm::vec2 p) {
    return p.x >= layout.lo.x && p.x <= layout.hi.x && p.y >= layout.lo.y && p.y <= layout.hi.y;
}

Hit hit_socket(const std::vector<NodeLayout>& layouts, glm::vec2 p, float tolerance) {
    Hit out;
    float best = tolerance;
    // Back to front: the node drawn last is the one on top, so its sockets win an overlap.
    for (auto it = layouts.rbegin(); it != layouts.rend(); ++it) {
        for (const auto& s : it->sockets) {
            const float d = glm::distance(p, s.pos);
            if (d > best) continue;
            best         = d;
            out.kind     = HitKind::Socket;
            out.node     = it->id;
            out.socket   = s.identifier;
            out.is_input = s.is_input;
        }
    }
    return out;
}

bool link_endpoints(const ng::Link& link, const std::vector<NodeLayout>& layouts,
                    glm::vec2& out_from, glm::vec2& out_to) {
    const NodeLayout* a = layout_of_(layouts, link.from.node);
    const NodeLayout* b = layout_of_(layouts, link.to.node);
    if (a == nullptr || b == nullptr) return false;
    const SocketLayout* sa = a->find_socket(link.from.socket, /*input=*/false);
    const SocketLayout* sb = b->find_socket(link.to.socket, /*input=*/true);
    // A link naming a socket the current declaration does not have is not drawn. That happens
    // legitimately: a node's sockets follow its props, so changing `data_type` can orphan a link
    // until the user reconnects it. validate() reports it; the canvas just has nothing to draw.
    if (sa == nullptr || sb == nullptr) return false;
    out_from = sa->pos;
    out_to   = sb->pos;
    return true;
}

int hit_link(const ng::Graph& graph, const std::vector<NodeLayout>& layouts, glm::vec2 p,
             float tolerance) {
    int best_index = -1;
    float best = tolerance;
    for (size_t i = 0; i < graph.links().size(); ++i) {
        glm::vec2 from, to;
        if (!link_endpoints(graph.links()[i], layouts, from, to)) continue;

        // Cheap reject on the curve's bounding box, widened by the control reach: a graph with a
        // few hundred wires would otherwise sample every one of them on every mouse move.
        const float reach = wire_tangent(from, to);
        const glm::vec2 lo = glm::min(from, to) - glm::vec2(reach, 0.0f) - glm::vec2(tolerance);
        const glm::vec2 hi = glm::max(from, to) + glm::vec2(reach, 0.0f) + glm::vec2(tolerance);
        if (p.x < lo.x || p.x > hi.x || p.y < lo.y || p.y > hi.y) continue;

        const std::vector<glm::vec2> pts = wire_points(from, to);
        for (size_t k = 1; k < pts.size(); ++k) {
            const float d = point_segment_distance(p, pts[k - 1], pts[k]);
            if (d < best) {
                best       = d;
                best_index = static_cast<int>(i);
            }
        }
    }
    return best_index;
}

Hit hit_test(const ng::Graph& graph, const std::vector<NodeLayout>& layouts,
             const LayoutMetrics& metrics, glm::vec2 p,
             float socket_tolerance, float link_tolerance) {
    if (Hit s = hit_socket(layouts, p, socket_tolerance)) return s;

    for (auto it = layouts.rbegin(); it != layouts.rend(); ++it) {
        if (!node_contains(*it, p)) continue;
        Hit out;
        // The header is reported separately: dragging a node by its title is the usual gesture,
        // and the body is where per-socket widgets will eventually live.
        out.kind = p.y <= it->lo.y + metrics.header_h ? HitKind::Header : HitKind::Node;
        out.node = it->id;
        return out;
    }

    const int link = hit_link(graph, layouts, p, link_tolerance);
    if (link >= 0) {
        Hit out;
        out.kind = HitKind::Link;
        out.link = link;
        return out;
    }
    return {};
}

std::vector<ng::NodeId> nodes_in_box(const std::vector<NodeLayout>& layouts, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 lo = glm::min(a, b);
    const glm::vec2 hi = glm::max(a, b);
    std::vector<ng::NodeId> out;
    for (const auto& l : layouts) {
        // Overlap, not containment: dragging a band across part of a node selects it, which is
        // what every other box-select in this editor does.
        if (l.hi.x < lo.x || l.lo.x > hi.x || l.hi.y < lo.y || l.lo.y > hi.y) continue;
        out.push_back(l.id);
    }
    return out;
}

} // namespace node
} // namespace editor
} // namespace toy
