/**
 * @file node_view.cpp
 * @brief fit_view(), the one piece of NodeView with a degenerate case worth isolating.
 */

#include "editor/node/node_view.h"

#include <algorithm>

namespace toy {
namespace editor {
namespace node {

NodeView fit_view(const imm::Box& viewport, glm::vec2 lo, glm::vec2 hi, float margin_px) {
    NodeView view;
    view.pan = (lo + hi) * 0.5f;

    const glm::vec2 span = hi - lo;
    const float w = std::max(1.0f, viewport.w - 2.0f * margin_px);
    const float h = std::max(1.0f, viewport.h - 2.0f * margin_px);
    // A graph with one node has zero span in both axes; a row of nodes has zero in one. Either
    // way the ratio for that axis is meaningless, so only a positive span constrains the zoom.
    float fit = 0.0f;
    if (span.x > 1e-3f) fit = w / span.x;
    if (span.y > 1e-3f) fit = fit > 0.0f ? std::min(fit, h / span.y) : h / span.y;
    view.zoom = fit > 0.0f ? std::clamp(fit, NodeView::k_min_zoom, NodeView::k_max_zoom) : 1.0f;
    return view;
}

} // namespace node
} // namespace editor
} // namespace toy
