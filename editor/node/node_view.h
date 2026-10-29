/**
 * @file node_view.h
 * @brief `NodeView` -- the pan/zoom transform between graph space and screen pixels.
 *
 * `imm` has no transform or matrix stack of any kind, so a zoomable canvas transforms every point
 * by hand. Keeping that arithmetic here rather than inline in the panel is what lets the
 * cursor-anchored zoom be unit-tested: "the point under the pointer does not move" is the property
 * that makes zooming feel right and is the easiest one to get subtly wrong.
 *
 * Graph space is what `Node::ui_position` is in and what the file stores as `pos:`. Screen space is
 * `imm` pixels, +Y down, with the canvas viewport's own origin already applied -- so `to_screen()`
 * hands back something a drawing call can use directly.
 */

#ifndef TOYENGINE_EDITOR_NODE_NODE_VIEW_H
#define TOYENGINE_EDITOR_NODE_NODE_VIEW_H

#include <glm/glm.hpp>

#include <uicoopa/immediate/imm.h>

namespace imm = coopa::ui::imm;

namespace toy {
namespace editor {
namespace node {

/**
 * @struct NodeView
 * @brief Where the graph is being looked at from.
 *
 * `pan` is the graph-space point shown at the viewport's centre, so zooming about the centre needs
 * no pan change at all -- the alternative (pan as a screen offset) has to be corrected on every
 * zoom and reads worse.
 */
struct NodeView {
    glm::vec2 pan{0.0f};    ///< The graph-space point at the viewport's centre.
    float     zoom = 1.0f;  ///< Screen pixels per graph unit.

    /// @brief The narrowest and widest the canvas may be zoomed.
    static constexpr float k_min_zoom = 0.1f;
    static constexpr float k_max_zoom = 4.0f;

    glm::vec2 to_screen(const imm::Box& viewport, glm::vec2 graph) const {
        return viewport.center() + (graph - pan) * zoom;
    }

    glm::vec2 to_graph(const imm::Box& viewport, glm::vec2 screen) const {
        return pan + (screen - viewport.center()) / zoom;
    }

    /// @brief A screen-space delta (a mouse drag) in graph units.
    glm::vec2 delta_to_graph(glm::vec2 screen_delta) const { return screen_delta / zoom; }

    /// @brief A graph-space rectangle as a screen Box.
    imm::Box box_of(const imm::Box& viewport, glm::vec2 lo, glm::vec2 hi) const {
        const glm::vec2 a = to_screen(viewport, lo);
        const glm::vec2 b = to_screen(viewport, hi);
        return imm::Box{a.x, a.y, b.x - a.x, b.y - a.y};
    }

    /**
     * @brief Scales about `anchor`, keeping the graph point under it fixed on screen.
     *
     * The clamp is applied before the pan correction, so a zoom that saturates still leaves the
     * anchor where it was rather than drifting by the clamped-away amount.
     */
    void zoom_at(const imm::Box& viewport, glm::vec2 anchor, float factor) {
        const glm::vec2 before = to_graph(viewport, anchor);
        zoom = glm::clamp(zoom * factor, k_min_zoom, k_max_zoom);
        const glm::vec2 after = to_graph(viewport, anchor);
        pan += before - after;
    }

    /// @brief Pans by a screen-space drag.
    void pan_by(glm::vec2 screen_delta) { pan -= delta_to_graph(screen_delta); }
};

/**
 * @brief The view that fits the graph-space box `lo..hi` into `viewport` with a margin.
 *
 * An empty or degenerate box (one node, or none) yields a centred view at zoom 1 rather than an
 * infinite or zero zoom -- "frame all" on an empty graph must still leave a usable canvas.
 */
NodeView fit_view(const imm::Box& viewport, glm::vec2 lo, glm::vec2 hi,
                  float margin_px = 40.0f);

} // namespace node
} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_NODE_NODE_VIEW_H
