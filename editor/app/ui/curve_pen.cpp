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
#include <functional>

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
    select(-1);
    drag_ = {};
    app_.mode_ = InteractionMode::Pen;
    return true;
}

void CurvePenPanel::end_pen_() {
    if (modal_.active()) { modal_.cancel(); app_.doc_.end_merge(); }
    if (drag_.active) app_.doc_.end_merge();
    target_ = 0;
    select(-1);
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

void CurvePenPanel::select(int i) {
    selection_.clear();
    active_ = i;
    if (i >= 0) selection_.push_back(static_cast<std::size_t>(i));
}

void CurvePenPanel::select_add(std::size_t i) {
    const auto it = std::find(selection_.begin(), selection_.end(), i);
    if (it != selection_.end()) {
        // Shift on an already-picked anchor removes it, as it does everywhere else.
        selection_.erase(it);
        active_ = selection_.empty() ? -1 : static_cast<int>(selection_.back());
        return;
    }
    selection_.push_back(i);
    active_ = static_cast<int>(i);
}

glm::vec3 CurvePenPanel::selection_pivot_() const {
    const cv::CurveData* c = curve();
    if (c == nullptr || selection_.empty()) return glm::vec3(0.0f);
    glm::vec3 sum(0.0f);
    std::size_t n = 0;
    for (std::size_t i : selection_) {
        if (i >= c->points.size()) continue;
        sum += c->points[i].position;
        ++n;
    }
    return n ? sum / static_cast<float>(n) : glm::vec3(0.0f);
}

void CurvePenPanel::start_modal_(ModalKind kind, const ViewProj& vp, glm::vec2 mouse,
                                 const cv::CurveData* base) {
    // `base` matters when an operator has just CHANGED the curve: commit_() goes through
    // apply_(), which defers the write, so the live component is still a frame behind. Reading
    // curve() here would snapshot the pre-edit curve and the modal's first frame would commit
    // that back -- silently undoing the edit that started it.
    const cv::CurveData* c = base ? base : curve();
    if (c == nullptr || selection_.empty()) return;
    modal_base_ = *c;
    modal_pivot_ = selection_pivot_();
    // The modal works in WORLD units -- its axis keys are world axes -- and the curve is local
    // to its object, so the pivot it is handed has to be world too.
    const glm::vec3 pivot = glm::vec3(to_world_() * glm::vec4(modal_pivot_, 1.0f));
    modal_.begin(kind, vp, pivot, glm::mat3(1.0f), mouse);
}

bool CurvePenPanel::run_modal_(imm::Context& ctx, const ViewProj& vp) {
    if (!modal_.active()) return false;
    cv::CurveData* live = live_curve_();
    if (live == nullptr || selection_.empty() || modal_base_.points.empty()) {
        modal_.cancel();
        return false;
    }
    const imm::FrameInput& in = ctx.input();
    const ModalKind kind = modal_.kind();

    modal_.snap_step = 0.25f;
    const auto outcome = modal_.update(vp, ctx.mouse(), in.keys, in.pressed[0], in.pressed[1],
                                       false, false, in.down[2], in.pressed[2]);
    ctx.consume_keyboard();

    // Always from the pose the modal STARTED at, never from last frame's: re-applying a rotation
    // to its own output compounds it, and the anchors spiral while the mouse sits still.
    const ModalTransform::Result r =
        outcome == ModalTransform::Outcome::Cancelled ? ModalTransform::Result{} : modal_.result();
    const glm::mat4 to_local = glm::inverse(to_world_());

    // One rule for all three, expressed about the selection's pivot. It falls out correctly for
    // a single anchor: the pivot IS that anchor, so its position holds still and only its
    // handles swing or stretch -- which is exactly how you aim a tangent.
    const glm::vec3 move_local = glm::vec3(to_local * glm::vec4(r.translate, 0.0f));
    const glm::vec3 axis_local = glm::length(r.rotate_axis) > 1e-6f
        ? glm::normalize(glm::vec3(to_local * glm::vec4(r.rotate_axis, 0.0f)))
        : glm::vec3(0.0f, 0.0f, 1.0f);
    const glm::quat spin = glm::angleAxis(glm::radians(r.rotate_deg), axis_local);

    auto place = [&](const glm::vec3& p) {
        switch (kind) {
            case ModalKind::Grab:   return p + move_local;
            case ModalKind::Rotate: return modal_pivot_ + spin * (p - modal_pivot_);
            case ModalKind::Scale:  return modal_pivot_ + (p - modal_pivot_) * r.scale;
            default:                return p;
        }
    };

    cv::CurveData next = modal_base_;
    for (std::size_t i : selection_) {
        if (i >= next.points.size()) continue;
        cv::CurvePoint& p = next.points[i];
        const cv::CurvePoint& base = modal_base_.points[i];
        p.position   = place(base.position);
        p.in_handle  = place(base.in_handle);
        p.out_handle = place(base.out_handle);
        // An anchor aimed or stretched by hand cannot also derive its handles from its
        // neighbours, so a rotate or a scale takes it off the derived modes.
        if (kind != ModalKind::Grab &&
            (p.mode == cv::HandleMode::Vector || p.mode == cv::HandleMode::Auto)) {
            p.mode = cv::HandleMode::Free;
        }
    }
    cv::refresh_handles(next);

    const char* label = kind == ModalKind::Grab   ? "Move Curve Points"
                      : kind == ModalKind::Rotate ? "Rotate Curve Points"
                                                  : "Scale Curve Points";
    const bool running = outcome == ModalTransform::Outcome::Running;
    commit_(next, label, running);
    if (!running) {
        app_.doc_.end_merge();
        modal_.cancel();
    }
    return true;
}

bool CurvePenPanel::extrude_selected_(const ViewProj& vp, glm::vec2 mouse) {
    cv::CurveData* live = live_curve_();
    if (live == nullptr || selection_.size() != 1) return false;
    const std::size_t from = selection_.front();
    if (from >= live->points.size()) return false;

    // Born on top of its parent and immediately handed to the grab, as mesh extrude does: the
    // new anchor is useless where it is, so the operator moves it rather than making you start.
    cv::CurveData next = *live;
    cv::CurvePoint born = next.points[from];
    born.mode = cv::HandleMode::Aligned;
    const std::size_t at = from + 1;
    next.points.insert(next.points.begin() + static_cast<std::ptrdiff_t>(at), born);

    select(static_cast<int>(at));
    commit_(next, "Extrude Curve Point", false);
    app_.doc_.end_merge();

    // From `next`, not from the live curve: the commit above has not landed yet.
    start_modal_(ModalKind::Grab, vp, mouse, &next);
    return true;
}

void CurvePenPanel::handle_pen_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
    cv::CurveData* live = live_curve_();
    if (live == nullptr) return;
    const imm::FrameInput& in = ctx.input();
    const glm::vec2 m = ctx.mouse();
    const glm::mat4 to_world = to_world_();

    // A running G / R owns the frame: its own keys are the axis locks.
    if (run_modal_(ctx, vp)) return;

    // --- keys ---
    using coopa::input::Key;
    // The object operators, on the anchor selection. Available whichever TOOL is active: the
    // mode is what decides you are editing anchors, exactly as E works in mesh Edit Mode no
    // matter which tool is picked.
    if (!selection_.empty() && !modal_.active()) {
        if (ctx.shortcut(Key::G)) { start_modal_(ModalKind::Grab, vp, m); return; }
        if (ctx.shortcut(Key::R)) { start_modal_(ModalKind::Rotate, vp, m); return; }
        if (ctx.shortcut(Key::S)) { start_modal_(ModalKind::Scale, vp, m); return; }
        if (ctx.shortcut(Key::E) && extrude_selected_(vp, m)) return;
    }
    if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete)) {
        if (!selection_.empty()) {
            cv::CurveData next = *live;
            // Descending, so each erase cannot shift the indices still to be removed.
            std::vector<std::size_t> doomed = selection_;
            std::sort(doomed.begin(), doomed.end(), std::greater<std::size_t>());
            for (std::size_t i : doomed) cv::remove_point(next, i);
            select(-1);
            commit_(next, doomed.size() > 1 ? "Delete Curve Points" : "Delete Curve Point", false);
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
            cv::CurveData next = drag_.base;
            if (drag_.index < next.points.size()) {
                // By TRAVEL, from the pose at press. `mouse - anchor` would teleport whatever is
                // grabbed to sit under the cursor on the first frame.
                const glm::vec3 ref = drag_.base.points[drag_.index].position;
                const glm::vec3 delta = place_at_(vp, m, ref) - drag_.press_world;
                if (glm::dot(delta, delta) > 1e-12f) drag_.moved = true;

                if (drag_.kind == ct::HitKind::Point) {
                    // Dragging one of several selected anchors moves them all, as dragging one
                    // of several selected objects does.
                    if (selection_.empty()) {
                        cv::move_point(next, drag_.index, delta);
                    } else {
                        for (std::size_t i : selection_) {
                            if (i < next.points.size()) cv::move_point(next, i, delta);
                        }
                    }
                } else {
                    const bool out = drag_.kind == ct::HitKind::OutHandle;
                    const glm::vec3 base_handle =
                        out ? drag_.base.points[drag_.index].out_handle
                            : drag_.base.points[drag_.index].in_handle;
                    ct::drag_handle(next, drag_.index, out, base_handle + delta);
                    drag_.moved = true;
                }
                // `active` while the button is down merges every frame into ONE undo step.
                if (drag_.moved) commit_(next, "Move Curve Point", true);
            }
            return;
        }
        if (drag_.moved) app_.doc_.end_merge();
        drag_ = {};
        return;
    }

    // --- rubber band in progress ---
    if (band_.pending || band_.active) {
        band_.now = m;
        if (!band_.active && glm::distance(band_.now, band_.press) > 4.0f) band_.active = true;
        if (in.down[0]) return;
        if (band_.active) {
            const auto picked = ct::anchors_in_box(*live, vp, to_world, band_.press, band_.now);
            if (!band_.additive) selection_.clear();
            for (std::size_t i : picked) {
                if (std::find(selection_.begin(), selection_.end(), i) == selection_.end()) {
                    selection_.push_back(i);
                }
            }
            active_ = selection_.empty() ? -1 : static_cast<int>(selection_.back());
        } else if (!band_.additive) {
            select(-1);   // a click, not a drag: clicking the void clears
        }
        band_ = {};
        return;
    }

    if (!hovered || ctx.popup_hovered()) return;
    if (!in.pressed[0]) return;

    const bool shift = has(in.mods, coopa::input::Mods::Shift);
    const ct::CurveHit hit = ct::hit_test(*live, vp, to_world, m, 9.0f, !selection_.empty());

    // Picking and handle dragging are the same under both tools -- what differs is only what a
    // click on NOTHING does.
    auto begin_drag = [&](const ct::CurveHit& h) {
        drag_ = {};
        drag_.active = true;
        drag_.kind = h.kind;
        drag_.index = h.index;
        drag_.base = *live;
        drag_.press_world = place_at_(vp, m, live->points[h.index].position);
    };

    if (hit.kind == ct::HitKind::Point) {
        if (shift) select_add(hit.index);
        else if (std::find(selection_.begin(), selection_.end(), hit.index) == selection_.end()) {
            select(static_cast<int>(hit.index));
        } else {
            active_ = static_cast<int>(hit.index);
        }
        begin_drag(hit);
        return;
    }
    if (hit.kind == ct::HitKind::InHandle || hit.kind == ct::HitKind::OutHandle) {
        begin_drag(hit);
        return;
    }
    if (hit.kind == ct::HitKind::Segment && tool_ == PenTool::Draw) {
        cv::CurveData next = *live;
        const std::size_t at = cv::insert_point(next, hit.index, hit.t);
        select(static_cast<int>(at));
        commit_(next, "Insert Curve Point", false);
        return;
    }

    if (tool_ != PenTool::Draw) {
        // Select tool on empty space: a rubber band, exactly as Box Select does on objects. It
        // only becomes a band once the mouse moves -- a press that does not travel is a click,
        // and a click on the void clears.
        band_ = {};
        band_.pending = true;
        band_.additive = shift;
        band_.press = m;
        band_.now = m;
        return;
    }

    // Draw tool on empty space: extend the curve. The anchor is the last point, so the
    // camera-plane rule has somewhere sensible to put a first point on an empty curve.
    cv::CurveData next = *live;
    const glm::vec3 anchor = next.points.empty() ? glm::vec3(0.0f) : next.points.back().position;
    const std::size_t at = cv::append_point(next, place_at_(vp, m, anchor));
    select(static_cast<int>(at));
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
        const bool is_sel = std::find(selection_.begin(), selection_.end(), i) != selection_.end();
        const bool is_active = static_cast<int>(i) == active_;
        const auto p = vp.project(glm::vec3(to_world * glm::vec4(c->points[i].position, 1.0f)));
        if (!p) continue;
        // Handles only on the selection, as in Blender: every handle at once is unreadable.
        if (is_active) {
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
    draw_band_(ctx);
}

void CurvePenPanel::draw_band_(imm::Context& ctx) const {
    if (!band_.active) return;
    const imm::Box r{std::min(band_.press.x, band_.now.x), std::min(band_.press.y, band_.now.y),
                     std::abs(band_.now.x - band_.press.x), std::abs(band_.now.y - band_.press.y)};
    ctx.fill(r, app_.et_.viewport.box_select_fill);
    ctx.outline(r, app_.et_.viewport.box_select_outline);
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

void CurvePenPanel::draw_pen_toolbar_(imm::Context& ctx, const imm::Box& r) {
    using I = imm::Icon;
    float y = r.y + 4;
    auto btn = [&](const char* id, I ic, const char* tip, bool on) {
        const imm::Box b{r.x + 3, y, 32.0f, 32.0f};
        y += 34.0f;
        return ctx.icon_button(id, ic, tip, on, 32.0f, imm::Context::kAll, b);
    };
    if (btn("pen_select", I::SelectBox,
            "Select\nClick an anchor to pick it, Shift to add. G moves, R rotates, S scales -- "
            "the same operators objects use.",
            tool_ == PenTool::Select)) {
        tool_ = PenTool::Select;
    }
    if (btn("pen_draw", I::Pen,
            "Pen\nClick to add an anchor at the end, or on the line to insert one without "
            "moving the shape.",
            tool_ == PenTool::Draw)) {
        tool_ = PenTool::Draw;
    }
    y += 6.0f;
    if (btn("pen_done", I::ObjectMode, "Object Mode\nLeave Pen Mode (Tab)", false)) end_pen_();
}

void CurvePenPanel::draw_pen_props_(imm::Context& ctx) {
    ctx.label("Pen");
    if (target_ == 0) {
        ctx.label("Select an object and press the Pen tool to draw a path on it.");
        return;
    }
    int which = tool_ == PenTool::Select ? 0 : 1;
    if (ctx.combo("Tool", &which, {"select", "draw"})) {
        tool_ = which == 0 ? PenTool::Select : PenTool::Draw;
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
    ctx.label(std::to_string(selection_.size()) + " selected");
    ctx.label("G moves, R rotates, S scales, E extrudes from one anchor.");
    ctx.label("X deletes the selection, C closes the loop.");
}

} // namespace editor
} // namespace toy
