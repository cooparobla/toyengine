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
#include <cstdio>
#include <functional>

#include <toyengine/curve/curve.h>
#include <toyengine/scene/runtime_object.h>
#include <toyengine/world/terrain_component.h>

#include "editor/app/editor_app.h"
#include "editor/app/ui/instances.h"

namespace toy {
namespace editor {

namespace cv = toy::curve;
namespace ct = curve_tool;

namespace {

/// The id of `parent`'s direct child called `name`, or 0.
ObjectId child_named_(SceneDocument& doc, ObjectId parent, const std::string& name) {
    const Node* obj = doc.find(parent);
    if (obj == nullptr || !obj->contains("children")) return 0;
    for (const Node& child : obj->at("children")) {
        if (child.contains("name") && child.at("name").get_value<std::string>() == name) {
            return SceneDocument::id_of(child);
        }
    }
    return 0;
}

/// The first component of `type` on `obj`, or null.
const Node* component_node_(const Node& obj, const char* type) {
    if (!obj.contains("components")) return nullptr;
    for (const Node& c : obj.at("components")) {
        if (c.contains("type") && c.at("type").get_value<std::string>() == type) return &c;
    }
    return nullptr;
}

/// The first object carrying a Curve anywhere under `root`, or 0.
ObjectId first_curve_under_(SceneDocument& doc, ObjectId root) {
    const Node* obj = doc.find(root);
    if (obj == nullptr) return 0;
    if (component_node_(*obj, "Curve") != nullptr) return root;
    if (!obj->contains("children")) return 0;
    for (const Node& child : obj->at("children")) {
        if (const ObjectId found = first_curve_under_(doc, SceneDocument::id_of(child))) return found;
    }
    return 0;
}

/// Every object carrying a Curve anywhere under `root`, depth first -- what the hair system
/// itself counts as this groom's guides.
void collect_curves_(SceneDocument& doc, ObjectId root, std::vector<ObjectId>& out) {
    const Node* obj = doc.find(root);
    if (obj == nullptr) return;
    if (component_node_(*obj, "Curve") != nullptr) out.push_back(root);
    if (!obj->contains("children")) return;
    for (const Node& child : obj->at("children")) {
        collect_curves_(doc, SceneDocument::id_of(child), out);
    }
}

/// Curve components anywhere under `root`, which is what the hair system counts as guides.
int count_curves_(SceneDocument& doc, ObjectId root) {
    const Node* obj = doc.find(root);
    if (obj == nullptr) return 0;
    int found = 0;
    if (obj->contains("components")) {
        for (const Node& c : obj->at("components")) {
            if (c.contains("type") && c.at("type").get_value<std::string>() == "Curve") ++found;
        }
    }
    if (obj->contains("children")) {
        for (const Node& child : obj->at("children")) {
            found += count_curves_(doc, SceneDocument::id_of(child));
        }
    }
    return found;
}


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

ObjectId CurvePenPanel::curve_under_(const ViewProj& vp, glm::vec2 mouse) const {
    ObjectId best = 0;
    float best_px = 1e9f;
    for (const auto& [id, live] : app_.sync_.live_objects()) {
        if (live == nullptr || id == target_) continue;
        const auto* component = live->get_component<cv::Curve>();
        if (component == nullptr || component->data.empty()) continue;
        const auto* tc = live->get_transform();
        const glm::mat4 to_world = tc ? tc->transform().get_world_matrix() : glm::mat4(1.0f);
        // Handles off: a curve that is not the target draws none, so hit-testing them would pick
        // a curve by something that is not on the screen.
        const ct::CurveHit hit =
            ct::hit_test(component->data, vp, to_world, mouse, 10.0f, /*show_handles=*/false);
        if (hit.kind == ct::HitKind::None || hit.distance_px >= best_px) continue;
        best_px = hit.distance_px;
        best = id;
    }
    return best;
}

void CurvePenPanel::focus_curve_(ObjectId id) {
    if (modal_.active()) { modal_.cancel(); app_.doc_.end_merge(); }
    if (drag_.active) app_.doc_.end_merge();
    drag_ = {};
    target_ = id;
    select(-1);
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

    // Pen on a HAIR object means "edit this groom's guides", not "give the scalp a path of its
    // own". A Hair component holds no curve -- its guides are Curve components on child objects
    // -- so without this, entering the mode on the thing you actually selected would add a stray
    // Curve to the scalp and silently edit nothing.
    if (curve_node_(*obj) == nullptr && component_node_(*obj, "Hair") != nullptr) {
        const ObjectId guide = first_or_new_hair_guide_(id);
        if (guide != 0) id = guide;
    }

    obj = app_.doc_.find(id);
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

void CurvePenPanel::refresh_surfaces_() {
    // Mirrors InstancePaintPanel::refresh_surfaces_() (editor/app/ui/instance_paint.cpp): a scene
    // walk plus a BVH build per distinct mesh shape, so it has to happen once a frame and not
    // once a click.
    const std::uint64_t frame = app_.engine_.frame_count() + 1;
    if (surfaces_frame_ == frame) return;
    surfaces_frame_ = frame;
    surfaces_.clear();

    if (mesh_rays_revision_ != app_.mesh_cache_epoch()) {
        mesh_rays_.clear();
        mesh_rays_revision_ = app_.mesh_cache_epoch();
    }

    for (const auto& [id, live] : app_.sync_.live_objects()) {
        if (live == nullptr || live->get_transform() == nullptr) continue;
        bool shown = true;
        for (const auto* a = live; a != nullptr && shown; a = a->parent()) shown = a->active();
        if (!shown) continue;
        // Runtime objects are generated output -- a terrain chunk, a groom's own strand mesh.
        // Drawing a guide onto the groom it belongs to would be circular.
        if (toy::scene::is_runtime_object(*live)) continue;

        if (auto* terrain = live->get_component<toy::world::TerrainComponent>()) {
            terrain->sync_field();
            if (!terrain->field.empty()) {
                Surface s;
                s.id = id;
                s.terrain = terrain->field_view();
                surfaces_.push_back(s);
            }
            continue;
        }

        const Node* node = app_.doc_.find(id);
        if (node == nullptr) continue;
        const EditorApp::CachedMesh* cm = app_.mesh_for_object_(*node);
        if (cm == nullptr) continue;
        char key[96];
        std::snprintf(key, sizeof(key), "%zu/%.4f,%.4f,%.4f/%.4f,%.4f,%.4f", cm->mesh.positions.size(),
                      cm->lo.x, cm->lo.y, cm->lo.z, cm->hi.x, cm->hi.y, cm->hi.z);
        auto it = mesh_rays_.find(key);
        if (it == mesh_rays_.end()) {
            auto mr = std::make_unique<MeshRay>();
            mr->mesh = cm->mesh;
            mr->bvh.build(mr->mesh);
            it = mesh_rays_.emplace(key, std::move(mr)).first;
        }
        Surface s;
        s.id = id;
        s.mesh = &it->second->mesh;
        s.bvh = &it->second->bvh;
        s.world = live->get_transform()->transform().get_world_matrix();
        surfaces_.push_back(s);
    }
}

std::optional<SurfaceHit> CurvePenPanel::surface_hit_(const ViewProj& vp, glm::vec2 px) {
    refresh_surfaces_();
    glm::vec3 o, d;
    vp.ray(px, o, d);
    SurfaceRaycaster rc(o, d);
    for (const Surface& s : surfaces_) {
        if (s.bvh != nullptr && s.mesh != nullptr) rc.consider_mesh(s.id, *s.mesh, *s.bvh, s.world);
        else if (s.terrain) rc.consider_terrain(s.id, s.terrain);
    }
    const SurfaceHit* best = rc.best();
    return best != nullptr ? std::optional<SurfaceHit>(*best) : std::nullopt;
}

glm::vec3 CurvePenPanel::place_at_(const ViewProj& vp, glm::vec2 mouse, const glm::vec3& anchor) {
    const glm::mat4 inv = glm::inverse(to_world_());
    glm::vec3 world;
    // Whatever mesh or terrain the cursor is actually over wins -- without this, a scalp that
    // stands clear of the ground (a sphere on a stand, a wall-mounted bust) ray-traces PAST it
    // and a guide's root lands on the floor behind it instead of on the surface it grows from.
    if (const auto hit = surface_hit_(vp, mouse)) {
        world = hit->point;
    } else if (on_ground_) {
        if (const auto ground = ct::point_on_ground(vp, mouse, ground_height_)) world = *ground;
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

void CurvePenPanel::set_selection_handle_mode_(cv::HandleMode mode) {
    if (selection_.empty()) return;
    const cv::CurveData* live = live_curve_();
    if (live == nullptr) return;
    cv::CurveData next = *live;
    for (std::size_t i : selection_) cv::set_handle_mode(next, i, mode);
    const char* label = mode == cv::HandleMode::Free ? "Set Anchor Corner"
                       : mode == cv::HandleMode::Aligned ? "Set Anchor Smooth"
                       : mode == cv::HandleMode::Vector ? "Set Anchor Vector"
                                                         : "Set Anchor Auto";
    commit_(next, label, false);
}

bool CurvePenPanel::selection_handle_mode_is_(cv::HandleMode mode) const {
    const cv::CurveData* c = curve();
    if (c == nullptr || selection_.empty()) return false;
    for (std::size_t i : selection_) {
        if (i >= c->points.size() || c->points[i].mode != mode) return false;
    }
    return true;
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
        // Select on ANOTHER curve's line: switch to it. The Pen edits one curve at a time, and
        // this is how you choose which -- a groom's guides included, since they are ordinary
        // Curve objects and need no case of their own.
        //
        // Only under Select. With the Draw tool a click on a line means "insert a point", and
        // losing a stroke to an accidental switch is worse than having to change tool first.
        if (const ObjectId other = curve_under_(vp, m)) {
            focus_curve_(other);
            app_.doc_.select(other);   // so the outliner and inspector follow what is being edited
            return;
        }

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
        // The "show handles" toggle widens that from just the active point to the whole
        // selection -- handy for judging several anchors' shapes after a Corner/Smooth click.
        if (is_active || (show_selected_handles_ && is_sel)) {
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

int CurvePenPanel::draw_scene_curves_(imm::Context& ctx, const ViewProj& vp) {
    int drawn = 0;
    // Curves that are selected, or that asked to always show. The Pen tool draws its own target
    // with handles, so it is skipped here to avoid drawing the line twice.
    for (const auto& [id, live] : app_.sync_.live_objects()) {
        if (live == nullptr || id == target_) continue;
        const auto* component = live->get_component<cv::Curve>();
        if (component == nullptr) continue;
        // In Pen Mode EVERY curve is drawn, not only the selected and always-show ones: the
        // Select tool switches to whichever one you click, so they all have to be visible to be
        // worth clicking. Outside the mode a curve is an ordinary overlay and keeps its own rule.
        if (!in_pen_mode_() && !component->always_show && !app_.doc_.is_selected(id)) continue;
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
        ++drawn;
    }
    return drawn;
}

void CurvePenPanel::draw_pen_toolbar_(imm::Context& ctx, const imm::Box& r) {
    using I = imm::Icon;
    float y = r.y + 4;
    auto btn = [&](const char* id, I ic, const char* tip, bool on) {
        const imm::Box b{r.x + 3, y, 32.0f, 32.0f};
        y += 34.0f;
        app_.test_rects_[id] = b;
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

    // Anchor modes: set the SELECTION's handle mode directly, the way Photoshop's pen options
    // bar does, rather than only reachable by demoting a mode through dragging a handle.
    auto mode_btn = [&](const char* id, I ic, const char* tip, cv::HandleMode mode) {
        if (btn(id, ic, tip, selection_handle_mode_is_(mode))) set_selection_handle_mode_(mode);
    };
    mode_btn("anchor_corner", I::AnchorCorner,
             "Corner\nMakes the selected anchor's handles independent -- a hard bend.",
             cv::HandleMode::Free);
    mode_btn("anchor_smooth", I::AnchorSmooth,
             "Smooth\nMirrors the selected anchor's handles opposite each other, keeping their "
             "own lengths, so the curve passes through smoothly.",
             cv::HandleMode::Aligned);
    mode_btn("anchor_vector", I::AnchorVector,
             "Vector\nPoints the selected anchor's handles straight at its neighbours -- a "
             "straight segment on either side.",
             cv::HandleMode::Vector);
    mode_btn("anchor_auto", I::AnchorAuto,
             "Auto\nDerives the selected anchor's handles from its neighbours (Catmull-Rom); "
             "dragging one by hand switches it back to Corner.",
             cv::HandleMode::Auto);

    if (btn("anchor_show_handles", show_selected_handles_ ? I::Eye : I::EyeClosed,
            "Show Handles\nDraws every selected anchor's handles, not only the last one picked.",
            show_selected_handles_)) {
        show_selected_handles_ = !show_selected_handles_;
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
    ctx.tooltip("Place on ground\nA new point first tries to land on whatever mesh or terrain is "
                "under the cursor. Only when there is nothing there does this decide: a level "
                "plane at the height below, rather than one facing the camera.");
    ctx.drag_float("Height", &ground_height_, 0.01f);

    const cv::CurveData* c = curve();
    if (c == nullptr) return;
    ctx.separator();
    ctx.label(std::to_string(c->points.size()) + " points, " +
              std::to_string(static_cast<int>(cv::length(*c) * 100.0f) / 100.0f) + " m");
    ctx.label(std::to_string(selection_.size()) + " selected");
    ctx.label("G moves, R rotates, S scales, E extrudes from one anchor.");
    ctx.label("X deletes the selection, C closes the loop.");
    ctx.label("Corner/Smooth/Vector/Auto set the selected anchors' handle mode; Show Handles "
              "reveals handles on the whole selection, not only the last one picked.");
    if (tool_ == PenTool::Select) ctx.label_dim("Click another curve's line to switch to it.");
}


// --- The Hair component's "Guides" section ------------------------------------------------------


EditResult CurvePenPanel::draw_hair_guides_(imm::Context& ctx, SectionContext& sc) {
    EditResult out;
    const ComponentSchema* schema = find_schema("Hair");

    for (const char* key : {"guides", "guide_influence", "guide_count"}) {
        if (schema != nullptr) {
            if (const FieldDesc* f = schema->field(key)) out.absorb(draw_field(ctx, *f, sc.comp, sc.env));
        }
        sc.handled(key);   // drawn here, so the generic pass must not draw it again
    }

    const std::string group = sc.comp.contains("guides") && sc.comp.at("guides").is_string()
                                  ? sc.comp.at("guides").get_value<std::string>()
                                  : std::string();

    // What the groom is ACTUALLY following, counted the same way the hair system counts it: every
    // Curve under the named object. Reading it off the document means the inspector says the same
    // thing whether or not the scene has been rebuilt yet this frame.
    const ObjectId group_id = group.empty() ? 0 : child_named_(app_.doc_, sc.object_id, group);
    std::vector<ObjectId> list;
    if (group_id != 0) collect_curves_(app_.doc_, group_id, list);

    if (list.empty()) {
        ctx.label_dim("No guides: the groom is purely procedural.");
    }

    // The guides themselves, so the set can be managed from the one place that knows it IS a set.
    // Each row's point count is there because an empty guide looks identical to a drawn one in a
    // list, and an empty one is the usual reason a groom is not following anything.
    for (const ObjectId id : list) {
        const Node* obj = app_.doc_.find(id);
        if (obj == nullptr) continue;
        const std::string name = obj->contains("name") ? obj->at("name").get_value<std::string>()
                                                       : std::string("guide");
        std::size_t points = 0;
        if (auto* live = app_.sync_.live(id)) {
            if (const auto* curve = live->get_component<cv::Curve>()) points = curve->data.points.size();
        }
        ctx.push_id(static_cast<int>(id));
        if (ctx.selectable(name + "  (" + std::to_string(points) + ")", id == target_)) {
            begin_pen_(id);   // picking a guide is asking to draw it
        }
        ctx.pop_id();
    }

    // One button for the whole flow -- make the group if it is missing, make a guide under it,
    // and drop into Pen Mode on it. Drawing a guide is the point; the object plumbing is not.
    const bool add = ctx.button("Add Guide");
    ctx.tooltip("Add Guide\nCreates a curve under this object and starts drawing it");
    // add_hair_guide_() writes `guides:` when it is not set yet, so this does not -- two writers
    // of one invariant is how they drift apart.
    if (add) add_hair_guide_(sc.object_id, group.empty() ? std::string("Guides") : group);

    // Duplicate and Delete act on whichever guide the Pen is on, which is the one highlighted
    // above -- so the buttons and the list cannot disagree about what they mean.
    const bool on_guide =
        target_ != 0 && std::find(list.begin(), list.end(), target_) != list.end();
    if (ctx.button("Duplicate Guide", -1, on_guide)) duplicate_hair_guide_(target_);
    ctx.tooltip("Duplicate Guide\nCopies the active guide with its shape, so a second can be nudged off the first");
    if (ctx.button("Delete Guide", -1, on_guide)) delete_hair_guide_(target_);
    ctx.tooltip("Delete Guide\nRemoves the active guide. The groom re-grows without it");
    return out;
}

ObjectId CurvePenPanel::duplicate_hair_guide_(ObjectId guide) {
    if (guide == 0) return 0;
    const std::vector<ObjectId> copies = app_.doc_.duplicate_objects({guide});
    if (copies.empty()) return 0;
    app_.after_structure_change_(copies.front());
    begin_pen_(copies.front());
    return copies.front();
}

void CurvePenPanel::delete_hair_guide_(ObjectId guide) {
    if (guide == 0) return;

    // Work out the survivor BEFORE deleting, so the Pen lands on a neighbour rather than on
    // nothing: the one after it, or the one before when it was last.
    ObjectId next = 0;
    if (const std::optional<ObjectId> group = app_.doc_.parent_of(guide)) {
        std::vector<ObjectId> siblings;
        collect_curves_(app_.doc_, *group, siblings);
        for (std::size_t i = 0; i < siblings.size(); ++i) {
            if (siblings[i] != guide) continue;
            if (i + 1 < siblings.size()) next = siblings[i + 1];
            else if (i > 0) next = siblings[i - 1];
            break;
        }
    }

    // Let go of it first: the Pen holds this id as its target and would otherwise keep editing
    // an object that no longer exists.
    if (target_ == guide) focus_curve_(0);
    app_.apply_(app_.doc_.delete_objects({guide}));
    if (next != 0) focus_curve_(next);
}

ObjectId CurvePenPanel::add_hair_guide_(ObjectId hair_id, const std::string& group_name) {
    ObjectId parent = child_named_(app_.doc_, hair_id, group_name);
    if (parent == 0) {
        parent = app_.doc_.add_object(app_.doc_.make_object(group_name), hair_id, -1,
                                      "Add Guide Group");
    }

    // Point the component at the group, here and nowhere else. A guide the component does not
    // name is a guide the hair system never looks at -- the groom stays procedural however many
    // curves are drawn into it, with nothing on screen to say why.
    if (const Node* obj = app_.doc_.find(hair_id)) {
        if (const Node* hair = component_node_(*obj, "Hair")) {
            const bool already = hair->contains("guides") && hair->at("guides").is_string() &&
                                 !hair->at("guides").get_value<std::string>().empty();
            if (!already) {
                Node edited = *hair;
                edited["guides"] = group_name;
                app_.instances_panel_->edit_component_(hair_id, edited, "Set Hair Guides");
            }
        }
    }
    Node guide = app_.doc_.make_object(app_.doc_.unique_name("guide", parent));
    Node curve = default_component("Curve");
    // A guide is useless unseen: unlike a general-purpose path, it exists only to be looked at
    // and edited, so it draws even outside Pen Mode and while something else is selected --
    // the same as every hand-authored guide in the demo scenes already does.
    curve["always_show"] = Node(true);
    guide["components"].as_seq().push_back(std::move(curve));
    const ObjectId id = app_.doc_.add_object(guide, parent, -1, "Add Guide");
    app_.after_structure_change_(id);
    begin_pen_(id);
    return id;
}

ObjectId CurvePenPanel::first_or_new_hair_guide_(ObjectId hair_id) {
    const Node* obj = app_.doc_.find(hair_id);
    if (obj == nullptr) return 0;

    // Whatever the component names, falling back to the conventional child so a groom authored
    // without `guides:` still gets its curves somewhere predictable.
    std::string group = "Guides";
    if (const Node* hair = component_node_(*obj, "Hair")) {
        if (hair->contains("guides") && hair->at("guides").is_string()) {
            const std::string named = hair->at("guides").get_value<std::string>();
            if (!named.empty()) group = named;
        }
    }

    if (const ObjectId group_id = child_named_(app_.doc_, hair_id, group)) {
        if (const ObjectId existing = first_curve_under_(app_.doc_, group_id)) return existing;
    }
    return add_hair_guide_(hair_id, group);   // none yet: make the first one
}

} // namespace editor
} // namespace toy
