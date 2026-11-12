/**
 * @file rect_transform.h
 * @brief Unity-style RectTransform component: anchors, pivot, and anchored layout.
 *
 * Does not extend coopa::util::Transform — that class is TRS-only (position/Euler
 * rotation/scale) and cannot express anchor-driven sizing. RectTransform owns its
 * own RectParams and composes a glm::mat3 world matrix for the optional local
 * rotation/scale it applies about its own pivot.
 *
 * Layout math (resolve_rect) always resolves against the parent's axis-aligned
 * rect; a rotated/scaled parent does not rotate its children's layout, only its
 * own rendered/hit-tested geometry. This is a deliberate simplification — Unity's
 * canvas layout has the same practical constraint for non-World-Space canvases.
 */

#ifndef UICOOPA_LAYOUT_RECT_TRANSFORM_H
#define UICOOPA_LAYOUT_RECT_TRANSFORM_H

#include <uicoopa/ui_component.h>
#include <uicoopa/layout/rect.h>
#include <glm/glm.hpp>
#include <array>
#include <cmath>
#include <string>

namespace coopa {
namespace ui {

/**
 * @enum AnchorPreset
 * @brief Named anchor/pivot combinations mirroring Unity's RectTransform anchor presets.
 */
enum class AnchorPreset {
    TopLeft, TopCenter, TopRight,
    MiddleLeft, MiddleCenter, MiddleRight,
    BottomLeft, BottomCenter, BottomRight,
    StretchAll, StretchTop, StretchBottom, StretchLeft, StretchRight,
    StretchHorizontal, StretchVertical,
};

/**
 * @class RectTransform
 * @brief Anchored rect layout component. Every UI SceneObject has exactly one.
 *
 * Usage:
 * @code
 * auto* rt = obj->add_component<RectTransform>();
 * rt->anchor_preset(AnchorPreset::TopLeft);
 * rt->set_anchored_position({20.0f, -20.0f});
 * rt->set_size_delta({200.0f, 60.0f});
 * @endcode
 */
class RectTransform : public UIComponent {
public:
    RectTransform() = default;

    std::string type_name() const override { return "RectTransform"; }

    /**
     * @brief Whether Raycaster considers this object (and, transitively, still
     *        recurses into its children) during hit testing.
     *
     * Distinct from Graphic::raycast_target: that flag is per-component and only
     * exists on Graphic-derived widgets, so it cannot silence a whole subtree or
     * a non-Graphic raycast target (ScrollRect, InventorySlot, ...). Setting this
     * false on a purely decorative node (a Button's Label, a slider's Track/Fill/
     * Handle, an inventory slot's Bg/Icon/Count) keeps it from shadowing the
     * interactive object underneath it. Defaults true so existing scenes are
     * unaffected.
     */
    bool hittable = true;

    /**
     * @brief Offset from the parent's EFFECTIVE z-order; 0 = inherit unchanged.
     *
     * Effective z-order accumulates additively down the tree: effective(child) =
     * effective(parent) + child.z_order. It is the primary sort key for both
     * Raycaster::hit_test_all() and CanvasComponent's emit pass — higher sorts on
     * top of lower — with the existing hierarchy-order rule (reverse-emit order)
     * as the tiebreak within an equal z-order, so a scene with every z_order left
     * at 0 behaves byte-for-byte as if this field didn't exist.
     *
     * Setting this to a NONZERO value also makes this object's whole subtree
     * escape any ancestor Mask's clip — for both drawing and hit-testing — as if
     * it had been reparented directly under the canvas root for clipping
     * purposes only; its resolved rect() is still computed relative to its real
     * parent, so it stays exactly where its anchors put it. This is what lets a
     * ComboBox popup (see UIBuilder::add_dropdown) hang below its row without
     * being cut off by an ancestor ScrollRect's Mask, and without later rows in
     * the same VerticalLayoutGroup stealing its clicks. A Mask inside the
     * escaped subtree still clips its own descendants normally.
     */
    int z_order = 0;

    // --- Anchors, pivot, position, size ---

    void set_anchor_min(const glm::vec2& v) { params_.anchor_min = v; }
    const glm::vec2& anchor_min() const { return params_.anchor_min; }

    void set_anchor_max(const glm::vec2& v) { params_.anchor_max = v; }
    const glm::vec2& anchor_max() const { return params_.anchor_max; }

    void set_pivot(const glm::vec2& v) { params_.pivot = v; }
    const glm::vec2& pivot() const { return params_.pivot; }

    void set_anchored_position(const glm::vec2& v) { params_.anchored_position = v; }
    const glm::vec2& anchored_position() const { return params_.anchored_position; }

    void set_size_delta(const glm::vec2& v) { params_.size_delta = v; }
    const glm::vec2& size_delta() const { return params_.size_delta; }

    /** @brief Offset of the resolved rect's bottom-left corner from its anchor point. */
    glm::vec2 offset_min() const { return coopa::ui::offset_min(parent_rect_, params_); }
    /** @brief Offset of the resolved rect's top-right corner from its anchor point. */
    glm::vec2 offset_max() const { return coopa::ui::offset_max(parent_rect_, params_); }

    /** @brief Sets offset_min, adjusting anchored_position/size_delta; anchors/pivot unchanged. */
    void set_offset_min(const glm::vec2& v) {
        coopa::ui::set_offsets(parent_rect_, params_, v, offset_max());
    }
    /** @brief Sets offset_max, adjusting anchored_position/size_delta; anchors/pivot unchanged. */
    void set_offset_max(const glm::vec2& v) {
        coopa::ui::set_offsets(parent_rect_, params_, offset_min(), v);
    }

    /** @brief Local rotation in degrees, applied about the pivot for rendering/hit-testing only. */
    void set_local_rotation_degrees(float degrees) { local_rotation_degrees_ = degrees; }
    float local_rotation_degrees() const { return local_rotation_degrees_; }

    /** @brief Local scale, applied about the pivot for rendering/hit-testing only. */
    void set_local_scale(const glm::vec2& s) { local_scale_ = s; }
    const glm::vec2& local_scale() const { return local_scale_; }

    /** @brief Direct access to the full parameter set, e.g. for layout groups. */
    RectParams& params() { return params_; }
    const RectParams& params() const { return params_; }

    /**
     * @brief Sets the cached size measurement from the most recent measure pass.
     *
     * Written by CanvasComponent's bottom-up measure pass; read by an ancestor
     * layout group when it later arranges this object as one of its children.
     */
    void set_measured(const SizeConstraints& measured) { measured_ = measured; }

    /** @brief The size this object reported (aggregated across its UIComponents) in the last measure pass. */
    const SizeConstraints& measured() const { return measured_; }

    /** @brief The last-resolved axis-aligned rect, in canvas pixel space. Valid after resolve(). */
    const Rect& rect() const { return rect_; }

    /** @brief The last-resolved parent rect this was resolved against. */
    const Rect& parent_rect() const { return parent_rect_; }

    /** @brief The cached local rotation/scale-about-pivot matrix. Identity unless rotated/scaled. */
    const glm::mat3& world_matrix() const { return world_matrix_; }

    /**
     * @brief Returns the four corners of rect(), transformed by world_matrix(), in canvas space.
     * @return Corners in order: bottom-left, bottom-right, top-right, top-left.
     */
    std::array<glm::vec2, 4> world_corners() const {
        std::array<glm::vec2, 4> local = {
            glm::vec2(rect_.min.x, rect_.min.y),
            glm::vec2(rect_.max.x, rect_.min.y),
            glm::vec2(rect_.max.x, rect_.max.y),
            glm::vec2(rect_.min.x, rect_.max.y),
        };
        std::array<glm::vec2, 4> world{};
        for (size_t i = 0; i < local.size(); ++i) {
            glm::vec3 p = world_matrix_ * glm::vec3(local[i], 1.0f);
            world[i] = glm::vec2(p.x, p.y);
        }
        return world;
    }

    /**
     * @brief Sets anchor_min/anchor_max/pivot from a named preset.
     *
     * Leaves anchored_position and size_delta untouched, matching Unity's default
     * (non-"also set position") anchor preset behavior.
     */
    void anchor_preset(AnchorPreset preset) {
        switch (preset) {
            case AnchorPreset::TopLeft:      params_.anchor_min = {0.0f, 1.0f}; params_.anchor_max = {0.0f, 1.0f}; params_.pivot = {0.0f, 1.0f}; break;
            case AnchorPreset::TopCenter:    params_.anchor_min = {0.5f, 1.0f}; params_.anchor_max = {0.5f, 1.0f}; params_.pivot = {0.5f, 1.0f}; break;
            case AnchorPreset::TopRight:     params_.anchor_min = {1.0f, 1.0f}; params_.anchor_max = {1.0f, 1.0f}; params_.pivot = {1.0f, 1.0f}; break;
            case AnchorPreset::MiddleLeft:   params_.anchor_min = {0.0f, 0.5f}; params_.anchor_max = {0.0f, 0.5f}; params_.pivot = {0.0f, 0.5f}; break;
            case AnchorPreset::MiddleCenter: params_.anchor_min = {0.5f, 0.5f}; params_.anchor_max = {0.5f, 0.5f}; params_.pivot = {0.5f, 0.5f}; break;
            case AnchorPreset::MiddleRight:  params_.anchor_min = {1.0f, 0.5f}; params_.anchor_max = {1.0f, 0.5f}; params_.pivot = {1.0f, 0.5f}; break;
            case AnchorPreset::BottomLeft:   params_.anchor_min = {0.0f, 0.0f}; params_.anchor_max = {0.0f, 0.0f}; params_.pivot = {0.0f, 0.0f}; break;
            case AnchorPreset::BottomCenter: params_.anchor_min = {0.5f, 0.0f}; params_.anchor_max = {0.5f, 0.0f}; params_.pivot = {0.5f, 0.0f}; break;
            case AnchorPreset::BottomRight:  params_.anchor_min = {1.0f, 0.0f}; params_.anchor_max = {1.0f, 0.0f}; params_.pivot = {1.0f, 0.0f}; break;
            case AnchorPreset::StretchAll:        params_.anchor_min = {0.0f, 0.0f}; params_.anchor_max = {1.0f, 1.0f}; params_.pivot = {0.5f, 0.5f}; break;
            case AnchorPreset::StretchTop:        params_.anchor_min = {0.0f, 1.0f}; params_.anchor_max = {1.0f, 1.0f}; params_.pivot = {0.5f, 1.0f}; break;
            case AnchorPreset::StretchBottom:     params_.anchor_min = {0.0f, 0.0f}; params_.anchor_max = {1.0f, 0.0f}; params_.pivot = {0.5f, 0.0f}; break;
            case AnchorPreset::StretchLeft:       params_.anchor_min = {0.0f, 0.0f}; params_.anchor_max = {0.0f, 1.0f}; params_.pivot = {0.0f, 0.5f}; break;
            case AnchorPreset::StretchRight:      params_.anchor_min = {1.0f, 0.0f}; params_.anchor_max = {1.0f, 1.0f}; params_.pivot = {1.0f, 0.5f}; break;
            case AnchorPreset::StretchHorizontal: params_.anchor_min = {0.0f, 0.5f}; params_.anchor_max = {1.0f, 0.5f}; params_.pivot = {0.5f, 0.5f}; break;
            case AnchorPreset::StretchVertical:   params_.anchor_min = {0.5f, 0.0f}; params_.anchor_max = {0.5f, 1.0f}; params_.pivot = {0.5f, 0.5f}; break;
        }
    }

    /**
     * @brief Resolves this rect against parent_rect and rebuilds the local world matrix.
     *
     * Driven by CanvasComponent's arrange pass — not intended to be called directly
     * by widget code. Calling it manually (e.g. in a headless test) is safe and
     * side-effect-free beyond updating this object's own cached state.
     *
     * @param parent_rect The resolved parent rect, in canvas pixel space.
     */
    void resolve(const Rect& parent_rect) {
        parent_rect_ = parent_rect;
        rect_ = resolve_rect(parent_rect, params_);

        glm::vec2 pivot_world = rect_.min + rect_.size() * params_.pivot;

        if (local_rotation_degrees_ == 0.0f && local_scale_ == glm::vec2(1.0f)) {
            world_matrix_ = glm::mat3(1.0f);
            return;
        }

        float rad = glm::radians(local_rotation_degrees_);
        float c = std::cos(rad), s = std::sin(rad);

        glm::mat3 to_origin(1.0f);
        to_origin[2][0] = -pivot_world.x;
        to_origin[2][1] = -pivot_world.y;

        glm::mat3 scale(1.0f);
        scale[0][0] = local_scale_.x;
        scale[1][1] = local_scale_.y;

        glm::mat3 rotate(1.0f);
        rotate[0][0] = c;  rotate[0][1] = s;
        rotate[1][0] = -s; rotate[1][1] = c;

        glm::mat3 from_origin(1.0f);
        from_origin[2][0] = pivot_world.x;
        from_origin[2][1] = pivot_world.y;

        world_matrix_ = from_origin * rotate * scale * to_origin;
    }

private:
    RectParams params_;
    Rect       parent_rect_{};
    Rect       rect_{};
    float      local_rotation_degrees_ = 0.0f;
    glm::vec2  local_scale_{1.0f};
    glm::mat3  world_matrix_{1.0f};
    SizeConstraints measured_{};
};

}  // namespace ui
}  // namespace coopa

#endif  // UICOOPA_LAYOUT_RECT_TRANSFORM_H
