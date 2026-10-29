/**
 * @file curve_pen.cpp
 * @brief The Pen tool's input handling and overlay.
 *
 * The keymap, chosen to match the rest of the editor rather than to be clever:
 *
 * | Input | Does |
 * |---|---|
 * | Click empty space | Append a control point at the end of the curve |
 * | Click the drawn line | Insert a point there, leaving the shape exactly as it was |
 * | Click a point | Select it (its handles appear) |
 * | Drag a point or handle | Move it; one undo step per drag, not per frame |
 * | X / Delete | Remove the selected point |
 * | C | Toggle closed |
 */

#include "editor/app/ui/curve_pen.h"

#include <algorithm>

#include <toyengine/curve/curve.h>

#include "editor/app/editor_app.h"
#include "editor/app/ui/instances.h"

namespace toy {
namespace editor {

namespace cv = toy::curve;
namespace ct = curve_tool;

namespace {

/// @brief The component node of type `Curve` on `obj`, or null.
const Node* curve_node_(const Node& obj) {
    if (!obj.contains("components") || !obj.at("components").is_sequence()) return nullptr;
    for (const auto& c : obj.at("components").as_seq()) {
        if (c.contains("type") && c.at("type").get_value<std::string>() == "Curve") return &c;
    }
    return nullptr;
}

/// @brief `CurveData` -> the component node the parser reads back.
Node to_node_(const cv::CurveData& data) {
    Node out = Node::mapping();
    out["type"] = Node(std::string("Curve"));
    out["closed"] = Node(data.closed);
    out["resolution"] = Node(static_cast<int>(data.resolution));
    Node points = Node::sequence();
    for (const auto& p : data.points) {
        Node pn = Node::mapping();
        auto vec = [](const glm::vec3& v) {
            Node n = Node::sequence();
            n.as_seq().push_back(Node(static_cast<double>(v.x)));
            n.as_seq().push_back(Node(static_cast<double>(v.y)));
            n.as_seq().push_back(Node(static_cast<double>(v.z)));
            return n;
        };
        pn["p"] = vec(p.position);
        pn["in"] = vec(p.in_handle);
        pn["out"] = vec(p.out_handle);
        pn["mode"] = Node(std::string(cv::handle_mode_name(p.mode)));
        if (std::abs(p.tilt) > 1e-6f) pn["tilt"] = Node(static_cast<double>(p.tilt));
        points.as_seq().push_back(std::move(pn));
    }
    out["points"] = std::move(points);
    return out;
}

} // namespace

bool CurvePenPanel::in_pen_mode_() const {
    return app_.mode_ == InteractionMode::Pen && target_ != 0;
}

const cv::CurveData* CurvePenPanel::curve() const {
    if (target_ == 0) return nullptr;
    auto* live = app_.sync_.live(target_);
    const auto* c = live ? live->get_component<cv::Curve>() : nullptr;
    return c ? &c->data : nullptr;
}

cv::CurveData* CurvePenPanel::live_curve_() {
    auto* live = target_ ? app_.sync_.live(target_) : nullptr;
    auto* c = live ? live->get_component<cv::Curve>() : nullptr;
    return c ? &c->data : nullptr;
}

glm::mat4 CurvePenPanel::to_world_() const {
    auto* live = target_ ? app_.sync_.live(target_) : nullptr;
    const auto* tc = live ? live->get_transform() : nullptr;
    return tc ? tc->transform().get_world_matrix() : glm::mat4(1.0f);
}

bool CurvePenPanel::begin_pen_(ObjectId id) {
    const Node* obj = app_.doc_.find(id);
    if (obj == nullptr) return false;
    if (curve_node_(*obj) == nullptr) {
        // Entering the Pen tool on an object with no path means "draw one here", so the
        // component is added rather than the mode refusing to open.
        Node fresh = Node::mapping();
        fresh["type"] = Node(std::string("Curve"));
        fresh["closed"] = Node(false);
        fresh["resolution"] = Node(12);
        fresh["points"] = Node::sequence();
        app_.apply_(app_.doc_.add_component(id, std::move(fresh)));
    }
    target_ = id;
    selected_ = -1;
    drag_ = {};
    app_.mode_ = InteractionMode::Pen;
    return true;
}

void CurvePenPanel::end_pen_() {
    if (drag_.active) app_.doc_.end_merge();
    target_ = 0;
    selected_ = -1;
    drag_ = {};
    if (app_.mode_ == InteractionMode::Pen) app_.mode_ = InteractionMode::Object;
}

void CurvePenPanel::commit_(const cv::CurveData& data, const std::string& label, bool active) {
    if (target_ == 0) return;
    const Node* obj = app_.doc_.find(target_);
    if (obj == nullptr) return;
    Node edited = to_node_(data);
    // Keep the keys the schema owns but the tool does not touch, so editing a point does not
    // silently reset `always_show`.
    if (const Node* existing = curve_node_(*obj)) {
        if (existing->contains("always_show")) edited["always_show"] = existing->at("always_show");
        if (existing->contains("id")) edited["id"] = existing->at("id");
    }
    app_.instances_panel_->edit_component_(target_, edited, label,
                                           active ? "curve:" + std::to_string(target_) : std::string());
}

glm::vec3 CurvePenPanel::place_at_(const ViewProj& vp, glm::vec2 mouse,
                                   const glm::vec3& anchor) const {
    const glm::mat4 inv = glm::inverse(to_world_());
    glm::vec3 world;
    if (on_ground_) {
        if (const auto hit = ct::point_on_ground(vp, mouse, ground_height_)) world = *hit;
        else world = ct::point_on_camera_plane(vp, mouse, glm::vec3(to_world_() * glm::vec4(anchor, 1.0f)));
    } else {
        world = ct::point_on_camera_plane(vp, mouse, glm::vec3(to_world_() * glm::vec4(anchor, 1.0f)));
    }
    return glm::vec3(inv * glm::vec4(world, 1.0f));
}

void CurvePenPanel::handle_pen_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
    cv::CurveData* live = live_curve_();
    if (live == nullptr) return;
    const imm::FrameInput& in = ctx.input();
    const glm::vec2 m = ctx.mouse();
    const glm::mat4 to_world = to_world_();

    // --- keys ---
    using coopa::input::Key;
    if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete)) {
        if (selected_ >= 0 && static_cast<std::size_t>(selected_) < live->points.size()) {
            cv::CurveData next = *live;
            cv::remove_point(next, static_cast<std::size_t>(selected_));
            selected_ = -1;
            commit_(next, "Delete Curve Point", false);
            return;
        }
    }
    if (ctx.shortcut(Key::C)) {
        cv::CurveData next = *live;
        next.closed = !next.closed;
        cv::refresh_handles(next);
        commit_(next, next.closed ? "Close Curve" : "Open Curve", false);
        return;
    }

    // --- drag in progress ---
    if (drag_.active) {
        if (in.down[0]) {
            cv::CurveData next = *live;
            if (drag_.index < next.points.size()) {
                const glm::vec3 anchor = next.points[drag_.index].position;
                const glm::vec3 target = place_at_(vp, m, anchor);
                if (drag_.kind == ct::HitKind::Point) {
                    cv::move_point(next, drag_.index, target - anchor);
                } else {
                    ct::drag_handle(next, drag_.index,
                                    drag_.kind == ct::HitKind::OutHandle, target);
                }
                drag_.moved = true;
                // `active` while the button is down merges every frame into ONE undo step.
                commit_(next, "Move Curve Point", true);
            }
            return;
        }
        if (drag_.moved) app_.doc_.end_merge();
        drag_ = {};
        return;
    }

    if (!hovered || ctx.popup_hovered()) return;
    if (!in.pressed[0]) return;

    const ct::CurveHit hit = ct::hit_test(*live, vp, to_world, m, 9.0f, selected_ >= 0);

    if (hit.kind == ct::HitKind::Point) {
        selected_ = static_cast<int>(hit.index);
        drag_ = {true, hit.kind, hit.index, false};
        return;
    }
    if (hit.kind == ct::HitKind::InHandle || hit.kind == ct::HitKind::OutHandle) {
        drag_ = {true, hit.kind, hit.index, false};
        return;
    }
    if (hit.kind == ct::HitKind::Segment) {
        cv::CurveData next = *live;
        const std::size_t at = cv::insert_point(next, hit.index, hit.t);
        selected_ = static_cast<int>(at);
        commit_(next, "Insert Curve Point", false);
        return;
    }

    // Empty space: extend the curve. The anchor is the last point, so the camera-plane rule has
    // somewhere sensible to put a first point on an empty curve.
    cv::CurveData next = *live;
    const glm::vec3 anchor = next.points.empty() ? glm::vec3(0.0f) : next.points.back().position;
    const std::size_t at = cv::append_point(next, place_at_(vp, m, anchor));
    selected_ = static_cast<int>(at);
    commit_(next, "Add Curve Point", false);
}

void CurvePenPanel::draw_pen_overlay_(imm::Context& ctx, const ViewProj& vp) {
    const cv::CurveData* c = curve();
    if (c == nullptr) return;
    const glm::mat4 to_world = to_world_();
    const glm::vec4 line_col = ctx.style.object_active;
    const glm::vec4 point_col = ctx.style.object_selected;
    const glm::vec4 handle_col = ctx.style.text_dim;

    // The path itself: the same polyline() a bake walks, so what is drawn is what is built.
    const std::vector<glm::vec3> line = cv::polyline(*c);
    std::optional<glm::vec2> prev;
    for (const glm::vec3& p : line) {
        const auto now = vp.project(glm::vec3(to_world * glm::vec4(p, 1.0f)));
        if (prev && now) ctx.line(*prev, *now, line_col, 2.0f);
        prev = now;
    }

    for (std::size_t i = 0; i < c->points.size(); ++i) {
        const bool is_sel = static_cast<int>(i) == selected_;
        const auto p = vp.project(glm::vec3(to_world * glm::vec4(c->points[i].position, 1.0f)));
        if (!p) continue;
        // Handles only on the selection, as in Blender: every handle at once is unreadable.
        if (is_sel) {
            for (bool out : {false, true}) {
                const glm::vec3& h = out ? c->points[i].out_handle : c->points[i].in_handle;
                const auto hp = vp.project(glm::vec3(to_world * glm::vec4(h, 1.0f)));
                if (!hp) continue;
                ctx.line(*p, *hp, handle_col, 1.0f);
                ctx.circle(*hp, 3.5f, handle_col);
            }
        }
        ctx.circle(*p, is_sel ? 5.0f : 3.5f, is_sel ? point_col : line_col);
    }
}

void CurvePenPanel::draw_scene_curves_(imm::Context& ctx, const ViewProj& vp) {
    // Curves that are selected, or that asked to always show. The Pen tool draws its own target
    // with handles, so it is skipped here to avoid drawing the line twice.
    for (const auto& [id, live] : app_.sync_.live_objects()) {
        if (live == nullptr || id == target_) continue;
        const auto* component = live->get_component<cv::Curve>();
        if (component == nullptr) continue;
        if (!component->always_show && !app_.doc_.is_selected(id)) continue;
        const auto* tc = live->get_transform();
        const glm::mat4 to_world = tc ? tc->transform().get_world_matrix() : glm::mat4(1.0f);
        const glm::vec4 col = app_.doc_.is_selected(id) ? ctx.style.object_selected
                                                        : ctx.style.text_dim;
        std::optional<glm::vec2> prev;
        for (const glm::vec3& p : cv::polyline(component->data)) {
            const auto now = vp.project(glm::vec3(to_world * glm::vec4(p, 1.0f)));
            if (prev && now) ctx.line(*prev, *now, col, 1.5f);
            prev = now;
        }
    }
}

void CurvePenPanel::draw_pen_props_(imm::Context& ctx) {
    ctx.label("Pen");
    if (target_ == 0) {
        ctx.label("Select an object and press the Pen tool to draw a path on it.");
        return;
    }
    ctx.checkbox("Place on ground", &on_ground_);
    ctx.tooltip("Place on ground\nNew points land on a level plane at the height below, rather "
                "than on a plane facing the camera. What you want for laying a path over terrain.");
    ctx.drag_float("Height", &ground_height_, 0.01f);

    const cv::CurveData* c = curve();
    if (c == nullptr) return;
    ctx.separator();
    ctx.label(std::to_string(c->points.size()) + " points, " +
              std::to_string(static_cast<int>(cv::length(*c) * 100.0f) / 100.0f) + " m");
    ctx.label("Click to add, click the line to insert, X deletes, C closes.");
}

} // namespace editor
} // namespace toy
