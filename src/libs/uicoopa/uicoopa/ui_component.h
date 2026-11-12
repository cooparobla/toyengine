/**
 * @file ui_component.h
 * @brief Base class for all uicoopa components, extending coopa::scene::Component.
 *
 * coopa::scene::Component only exposes start()/update(dt)/type_name() — no layout,
 * draw, or raycast hooks. UIComponent adds the three hooks every UI component needs,
 * driven explicitly by CanvasComponent::rebuild() rather than by SceneObject's
 * pre-order update() walk (see canvas.h for why: layout groups need a bottom-up
 * measure pass that plain pre-order traversal cannot express).
 */

#ifndef UICOOPA_UI_COMPONENT_H
#define UICOOPA_UI_COMPONENT_H

#include <coopa/scene/component.h>
#include <uicoopa/layout/rect.h>
#include <glm/glm.hpp>

namespace coopa {
namespace ui {

class DrawList;  // Defined in uicoopa/render/draw_list.h.

/**
 * @struct SizeConstraints
 * @brief A component's reported layout sizing, analogous to Unity's ILayoutElement.
 *
 * flexible expresses how much of any leftover space along an axis this element
 * wants relative to its siblings (0 = none, matching Unity's flexibleWidth/Height).
 */
struct SizeConstraints {
    glm::vec2 min{0.0f};       /**< Minimum size the element can be shrunk to. */
    glm::vec2 preferred{0.0f}; /**< Size the element wants at its natural content size. */
    glm::vec2 flexible{0.0f};  /**< Relative share of leftover space along each axis. */
};

/**
 * @class UIComponent
 * @brief Base class for components that participate in canvas layout and rendering.
 *
 * Subclasses override whichever hooks are relevant: RectTransform overrides none of
 * these (it drives layout itself); Graphic-derived widgets override emit(); layout
 * groups typically override on_rect_changed() to re-arrange their children.
 */
class UIComponent : public coopa::scene::Component {
public:
    ~UIComponent() override = default;

    /**
     * @brief Called by CanvasComponent::rebuild() after this object's rect is resolved.
     * @param rect The newly resolved rect, in canvas pixel space.
     */
    virtual void on_rect_changed(const Rect& rect) { (void)rect; }

    /**
     * @brief Called by CanvasComponent::rebuild() during the emit pass.
     *
     * Graphic components append their geometry to draw_list here.
     * @param draw_list The canvas's draw list to append geometry to.
     */
    virtual void emit(DrawList& draw_list) { (void)draw_list; }

    /**
     * @brief Called by CanvasComponent::rebuild() during the emit pass, after this
     *        object's children (and their descendants) have all been emitted.
     *
     * Mirrors emit() to let a component wrap its subtree's geometry — e.g. Mask
     * pushes a clip rect in emit() and pops it here, so the push/pop bracket the
     * exact span of draw calls contributed by its children.
     * @param draw_list The canvas's draw list.
     */
    virtual void on_children_emitted(DrawList& draw_list) { (void)draw_list; }

    /**
     * @brief Whether this component should be considered by the raycaster.
     * @return True if this component can receive pointer events.
     */
    virtual bool wants_raycast() const { return false; }

    /**
     * @brief Called by CanvasComponent::rebuild() during the bottom-up measure pass.
     *
     * Overridden by LayoutElement and layout groups to report sizing; other
     * components contribute nothing by default.
     * @return This component's size contribution.
     */
    virtual SizeConstraints measure() const { return SizeConstraints{}; }
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_UI_COMPONENT_H
