#include "editor/app/ui/instance_paint.h"

#include "editor/app/editor_app.h"

#include <chrono>
#include <cmath>
#include <random>

#include <toyengine/instance/instance_painter_system.h>
#include <toyengine/scene/runtime_object.h>
#include <toyengine/world/terrain_component.h>

namespace toy {
namespace editor {

using toy::instance::ChunkCoord;
using toy::instance::InstancePainter;
using toy::instance::InstancePrototype;
using toy::instance::InstanceRecord;
using toy::instance::InstanceSetData;
using toy::instance::InstanceSetStore;

namespace {

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** @brief Seconds of holding the mouse still between repeated dabs, as the terrain brush does. */
constexpr double k_hold_dab_interval = 1.0 / 15.0;
double g_last_dab_time = 0.0;
glm::vec3 g_last_dab_world{0.0f};

struct ToolButton {
    InstanceTool tool;
    imm::Icon icon;
    const char* tip;
};

const ToolButton k_tools[] = {
    {InstanceTool::Paint, imm::Icon::BrushDraw,
     "Paint\nScatter instances under the brush at Density; Ctrl erases (1).\n"
     "Thins toward the rim, so a stroke has no hard edge"},
    {InstanceTool::Erase, imm::Icon::BrushGrab, "Erase\nRemove the instances under the brush (2)"},
    {InstanceTool::Place, imm::Icon::Object,
     "Place\nOne instance per click, exactly under the cursor (3).\n"
     "With Snap on, this is the tool for fences, walls and modular kits"},
    {InstanceTool::Cluster, imm::Icon::Dots,
     "Cluster\nOne click drops a clump of Count instances within Spread (4).\n"
     "Copses of trees, rock piles, patches of mushrooms"},
    {InstanceTool::Select, imm::Icon::SelectBox,
     "Select\nClick an instance, or drag a box over several (5).\n"
     "Shift adds, Ctrl removes. Then turn, rescale, raise or delete them in Properties > Tool"},
};

} // namespace

// =================================================================================
// Lifecycle
// =================================================================================

bool InstancePaintPanel::is_painter_object_(ObjectId id) const {
    return id != 0 && app_.doc_.find_component(id, "InstancePainter") >= 0;
}

InstancePainter* InstancePaintPanel::painter_live_() const {
    if (instance_object_ == 0) return nullptr;
    coopa::scene::SceneObject* live = app_.sync_.live(instance_object_);
    return live != nullptr ? live->get_component<InstancePainter>() : nullptr;
}

void InstancePaintPanel::set_instance_mode(bool on) {
    if (on) enter_instance_mode_(app_.doc_.primary()); else exit_instance_mode_();
}

void InstancePaintPanel::enter_instance_mode_(ObjectId id) {
    if (!is_painter_object_(id) || app_.playing()) {
        app_.log_warn("Instance Paint Mode: select an object with an InstancePainter component first");
        return;
    }
    if (app_.interaction_mode() != InteractionMode::Object) app_.set_interaction_mode(InteractionMode::Object);
    instance_mode_ = true;
    instance_object_ = id;
    app_.tool_ = EditorApp::Tool::Select;
    app_.prop_tab_ = PropTab::Tool;
    app_.doc_.clear_selection();
    app_.doc_.select(id, true);
    if (InstancePainter* p = painter_live_()) {
        brush_.prototype = std::clamp(brush_.prototype, 0, std::max(0, static_cast<int>(p->prototypes.size()) - 1));
        if (p->prototypes.empty()) {
            app_.log_info("This painter has no prototypes yet -- add one under Properties > Tool > Prototypes");
        }
    }
}

void InstancePaintPanel::exit_instance_mode_() {
    if (stroke_.active) instance_end_stroke_();
    resize_.active = false;
    instance_mode_ = false;
    instance_object_ = 0;
    mesh_rays_.clear();
    surfaces_.clear();
    surfaces_frame_ = 0;
    selection_.clear();
    select_drag_ = SelectDrag{};
}

void InstancePaintPanel::instance_frame_() {
    if (!instance_mode_) return;
    if (app_.playing() || !app_.doc_.find(instance_object_) || !is_painter_object_(instance_object_)) {
        exit_instance_mode_();
        return;
    }
    sync_selection_();
}

// =================================================================================
// Surfaces
// =================================================================================

void InstancePaintPanel::refresh_surfaces_() {
    // A dab fires one ray per candidate and the overlay ring fires 48 more; walking the scene
    // for each of them would make a stroke quadratic in the brush's density for no reason.
    // The frame counter starts at 0, so "built this frame" is its own flag rather than a
    // non-empty list -- a scene with nothing paintable in it must not re-walk on every ray.
    const std::uint64_t frame = app_.engine_.frame_count() + 1;
    if (surfaces_frame_ == frame) return;
    surfaces_frame_ = frame;
    surfaces_.clear();

    // A mesh edited or a scene rebuilt empties the editor's CPU mesh cache; the BVHs built from
    // it have to go at the same moment or a brush would keep hitting the old shape.
    if (mesh_rays_revision_ != app_.mesh_cache_epoch()) {
        mesh_rays_.clear();
        mesh_rays_revision_ = app_.mesh_cache_epoch();
    }

    for (const auto& [id, live] : app_.sync_.live_objects()) {
        if (live == nullptr || live->get_transform() == nullptr) continue;
        bool shown = true;
        for (const auto* a = live; a != nullptr && shown; a = a->parent()) shown = a->active();
        if (!shown) continue;
        // Runtime objects are things systems made: terrain chunk meshes, this painter's own
        // colliders. Painting onto them would mean painting onto the painter's output.
        if (toy::scene::is_runtime_object(*live)) continue;

        if (auto* terrain = live->get_component<toy::world::TerrainComponent>()) {
            terrain->sync_field();
            if (!terrain->field.empty()) {
                Surface s;
                s.id = id;
                s.terrain = terrain->field_view();
                surfaces_.push_back(s);
            }
            continue;   // its chunks are runtime objects, already skipped above
        }

        const Node* node = app_.doc_.find(id);
        if (node == nullptr) continue;
        const EditorApp::CachedMesh* cm = app_.mesh_for_object_(*node);
        if (cm == nullptr) continue;
        // One BVH per distinct mesh SHAPE, keyed by its vertex count and bounds rather than by
        // the object -- a hundred instances of one crate share a single tree.
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

void InstancePaintPanel::aim_(SurfaceRaycaster& rc) const {
    for (const Surface& s : surfaces_) {
        if (s.bvh != nullptr && s.mesh != nullptr) rc.consider_mesh(s.id, *s.mesh, *s.bvh, s.world);
        else if (s.terrain) rc.consider_terrain(s.id, s.terrain);
    }
}

std::optional<SurfaceHit> InstancePaintPanel::drop_onto_surface_(glm::vec2 xy, float from_z) {
    // Straight down from above: a candidate beside the cursor may sit on a different surface,
    // and the camera ray through it would meet that surface somewhere else entirely.
    refresh_surfaces_();
    SurfaceRaycaster down(glm::vec3(xy, from_z), glm::vec3(0.0f, 0.0f, -1.0f));
    aim_(down);
    const SurfaceHit* best = down.best();
    return best != nullptr ? std::optional<SurfaceHit>(*best) : std::nullopt;
}

std::optional<SurfaceHit> InstancePaintPanel::surface_hit_(const ViewProj& vp, glm::vec2 px) {
    refresh_surfaces_();
    glm::vec3 o, d;
    vp.ray(px, o, d);
    SurfaceRaycaster rc(o, d);
    aim_(rc);
    const SurfaceHit* best = rc.best();
    return best != nullptr ? std::optional<SurfaceHit>(*best) : std::nullopt;
}

// =================================================================================
// Strokes
// =================================================================================

void InstancePaintPanel::remember_chunk_(ChunkCoord c) {
    const auto key = std::make_pair(c.x, c.y);
    if (!stroke_.touched.insert(key).second) return;
    const std::vector<InstanceRecord>* records = stroke_.data->records(c);
    stroke_.before.emplace_back(c, records != nullptr ? *records : std::vector<InstanceRecord>{});
}

void InstancePaintPanel::instance_dab_at_(const ViewProj& vp, glm::vec2 px) {
    InstancePainter* painter = painter_live_();
    if (painter == nullptr || !stroke_.data) return;
    const std::optional<SurfaceHit> hit = surface_hit_(vp, px);
    if (!hit) return;
    g_last_dab_time = now_seconds();
    g_last_dab_world = hit->point;
    const std::uint32_t dab_seed = stroke_.seed ^ (stroke_.dab++ * 0x9E3779B9u);

    if (stroke_.erase || stroke_.tool == InstanceTool::Erase) {
        const int only = brush_.erase_active_only ? brush_.prototype : -1;
        for (ChunkCoord c : stroke_.data->chunks_in_radius(hit->point, brush_.radius)) remember_chunk_(c);
        stroke_.data->erase_in_radius(hit->point, brush_.radius, only);
        return;
    }

    if (painter->prototypes.empty()) return;
    const int palette = static_cast<int>(painter->prototypes.size());

    // The candidate xy points this dab offers, then one ray each: the surface under a candidate
    // is not the surface under the brush's centre, which is the whole point of painting in 3D.
    std::vector<glm::vec2> candidates;
    switch (stroke_.tool) {
        case InstanceTool::Paint:   candidates = brush_candidates(brush_, glm::vec2(hit->point), dab_seed); break;
        case InstanceTool::Cluster: candidates = cluster_candidates(brush_, glm::vec2(hit->point), dab_seed); break;
        case InstanceTool::Place:
            candidates.push_back(brush_.snap ? snap_to_grid(brush_, glm::vec2(hit->point)) : glm::vec2(hit->point));
            break;
        case InstanceTool::Erase: return;
    }

    const float drop_from = hit->point.z + std::max(20.0f, brush_.radius * 2.0f);
    for (std::size_t k = 0; k < candidates.size(); ++k) {
        const glm::vec2 xy = candidates[k];
        const std::uint32_t seed = dab_seed ^ (static_cast<std::uint32_t>(k) * 0x85EBCA6Bu + 0x165667B1u);
        int proto_index = brush_.prototype;
        if (stroke_.tool == InstanceTool::Cluster) proto_index = cluster_prototype(brush_, palette, seed);
        proto_index = std::clamp(proto_index, 0, palette - 1);
        const InstancePrototype& proto = painter->prototypes[static_cast<std::size_t>(proto_index)];

        const std::optional<SurfaceHit> landed = drop_onto_surface_(xy, drop_from);
        if (!landed) continue;

        if (brush_.min_spacing > 0.0f && stroke_.data->any_within(landed->point, brush_.min_spacing)) continue;
        if (brush_.snap && brush_.one_per_cell) {
            const float cell = std::max(brush_.grid_cell, 1e-3f);
            // Half a cell: one instance per cell, measured from the cell's own centre.
            if (stroke_.data->any_within(landed->point, cell * 0.5f)) continue;
        }

        SurfaceSample sample;
        sample.position = landed->point;
        sample.normal = landed->normal;
        const InstanceRecord r = make_record(proto, proto_index, sample, brush_, seed);
        remember_chunk_(stroke_.data->chunk_of(r.position));
        stroke_.data->add(r);
    }
}

void InstancePaintPanel::instance_begin_stroke_(const ViewProj& vp, glm::vec2 px, bool ctrl) {
    InstancePainter* painter = painter_live_();
    if (painter == nullptr || !painter->data) return;
    stroke_ = InstanceStroke{};
    stroke_.active = true;
    stroke_.erase = ctrl;
    stroke_.tool = brush_.tool;
    stroke_.data = painter->data;
    stroke_.seed = static_cast<std::uint32_t>(std::random_device{}());
    stroke_.last_px = px;
    g_last_dab_world = glm::vec3(1e30f);   // the first dab always lands
    instance_dab_at_(vp, px);
}

void InstancePaintPanel::instance_continue_stroke_(const ViewProj& vp, glm::vec2 px) {
    // Place and Cluster are click tools: dragging one must not carpet the ground.
    if (stroke_.tool == InstanceTool::Place || stroke_.tool == InstanceTool::Cluster) {
        stroke_.last_px = px;
        return;
    }
    // Dabs along the cursor's path, spaced by a fraction of the brush -- in metres on the
    // surface, not in pixels, so the spacing is the same near and far.
    const glm::vec2 from = stroke_.last_px;
    const float len = glm::distance(px, from);
    const int steps = std::max(1, static_cast<int>(len / 3.0f));
    const float spacing = std::max(0.05f, brush_.spacing * brush_.radius);
    for (int k = 1; k <= steps; ++k) {
        const glm::vec2 p = from + (px - from) * (static_cast<float>(k) / static_cast<float>(steps));
        const std::optional<SurfaceHit> hit = surface_hit_(vp, p);
        if (!hit) continue;
        if (glm::distance(hit->point, g_last_dab_world) >= spacing) instance_dab_at_(vp, p);
    }
    stroke_.last_px = px;
}

void InstancePaintPanel::instance_end_stroke_() {
    if (!stroke_.active) return;
    stroke_.active = false;
    if (!stroke_.data || stroke_.before.empty()) {
        stroke_ = InstanceStroke{};
        return;
    }
    InstancePatch before;
    before.data = stroke_.data;
    before.chunks = stroke_.before;
    std::vector<ChunkCoord> coords;
    coords.reserve(stroke_.before.size());
    for (const auto& [c, records] : stroke_.before) coords.push_back(c);
    InstancePatch after = capture_instance_patch(stroke_.data, coords);
    // Nothing actually changed (every candidate was rejected): not an undo step.
    bool changed = false;
    for (std::size_t i = 0; i < coords.size() && !changed; ++i) {
        changed = before.chunks[i].second.size() != after.chunks[i].second.size();
    }
    if (changed) {
        instance_undo_.set_budget(64u << 20, [](const InstancePatch& p) { return p.bytes(); }, 8);
        instance_undo_.push(std::string("Instance ") + instance_tool_name(stroke_.tool), std::move(before),
                            std::move(after));
    }
    stroke_ = InstanceStroke{};
}

void InstancePaintPanel::instance_apply_undo_(bool redo) {
    const InstancePatch* p = redo ? instance_undo_.redo() : instance_undo_.undo();
    if (p != nullptr && !apply_instance_patch(*p)) {
        app_.log_warn("Instance undo skipped: the painter was removed since");
    }
}

void InstancePaintPanel::save_instances_() {
    const int n = InstanceSetStore::instance().save_dirty();
    if (n > 0) app_.log_info("Saved " + std::to_string(n) + (n == 1 ? " instance set" : " instance sets"));
}

// =================================================================================
// Selecting
// =================================================================================

namespace {

/// How near the cursor an instance's screen position must be for a click to take it.
constexpr float k_pick_radius_px = 22.0f;
/// Slack around the viewport when rejecting off-screen instances, in pixels.
constexpr float k_pick_margin_px = 64.0f;

} // namespace

// One projection per painted instance. That is O(everything the painter holds), and it is fine
// because this runs on a click or on the release of a box drag, never per frame -- the overlay
// draws from the selection's own refs. If a world-sized painter ever makes it felt, the answer
// is to ask the render cache which chunks it actually built rather than to guess with a radius.
void InstancePaintPanel::gather_pickables_(const ViewProj& vp, std::vector<Pickable>& out) const {
    const InstancePainter* painter = painter_live_();
    if (painter == nullptr || !painter->data) return;
    const glm::vec3 eye = vp.camera_pos();
    const glm::vec3 fwd = vp.camera_forward();
    // What you can SEE is what you can click: an instance is rejected by its projection falling
    // outside the viewport, never by a distance from the camera. A metre radius would be wrong
    // at every zoom level -- framing a whole terrain puts all of it hundreds of metres away.
    const imm::Box& rect = vp.rect;

    for (ChunkCoord c : painter->data->chunks()) {
        const std::vector<InstanceRecord>* records = painter->data->records(c);
        if (records == nullptr) continue;
        for (std::uint32_t i = 0; i < records->size(); ++i) {
            const InstanceRecord& r = (*records)[i];
            if (r.hidden()) continue;
            // The visual centre, not the origin: an instance stands ON its position, so
            // projecting that alone would put a tree's hit point at its roots.
            float centre_z = 0.0f;
            if (const InstancePrototype* proto = painter->prototype_at(r.prototype)) {
                centre_z = 0.5f * (proto->bounds_min.z + proto->bounds_max.z) * r.scale.z;
            }
            const glm::vec3 world = r.position + glm::vec3(0.0f, 0.0f, centre_z);
            const auto screen = vp.project(world);
            if (!screen) continue;   // behind the camera
            if (screen->x < rect.x - k_pick_margin_px || screen->x > rect.right() + k_pick_margin_px ||
                screen->y < rect.y - k_pick_margin_px || screen->y > rect.bottom() + k_pick_margin_px) {
                continue;
            }
            out.push_back({toy::instance::InstanceRef{c, i}, *screen, glm::dot(world - eye, fwd)});
        }
    }
}

std::optional<toy::instance::InstanceRef> InstancePaintPanel::pick_instance_(const ViewProj& vp,
                                                                            glm::vec2 px) const {
    std::vector<Pickable> candidates;
    gather_pickables_(vp, candidates);
    const Pickable* best = nullptr;
    float best_score = k_pick_radius_px;
    for (const Pickable& p : candidates) {
        const float d = glm::distance(p.screen, px);
        if (d > k_pick_radius_px) continue;
        // Nearest to the cursor wins; the nearer of two at the same spot breaks the tie, so
        // clicking a thicket takes the tree in front rather than one behind it.
        if (d < best_score - 1.0f || (best != nullptr && d < best_score + 1.0f && p.depth < best->depth)) {
            best = &p;
            best_score = std::min(best_score, d);
        }
    }
    return best != nullptr ? std::optional<toy::instance::InstanceRef>(best->ref) : std::nullopt;
}

void InstancePaintPanel::apply_select_drag_(const ViewProj& vp) {
    if (!select_drag_.add && !select_drag_.subtract) selection_.clear();

    if (!select_drag_.is_box()) {
        // A click: one instance, or (on empty space, replacing) nothing.
        if (const auto ref = pick_instance_(vp, select_drag_.to)) {
            if (select_drag_.subtract) selection_.remove(*ref);
            else selection_.add(*ref);
        }
    } else {
        const glm::vec2 lo = glm::min(select_drag_.from, select_drag_.to);
        const glm::vec2 hi = glm::max(select_drag_.from, select_drag_.to);
        std::vector<Pickable> candidates;
        gather_pickables_(vp, candidates);
        for (const Pickable& p : candidates) {
            if (p.screen.x < lo.x || p.screen.x > hi.x || p.screen.y < lo.y || p.screen.y > hi.y) continue;
            if (select_drag_.subtract) selection_.remove(p.ref);
            else selection_.add(p.ref);
        }
    }

    // Remember the records as they are now, so an edit from elsewhere invalidates the refs
    // rather than silently addressing whatever slid into their indices.
    if (const InstancePainter* painter = painter_live_()) {
        if (painter->data) selection_revision_ = painter->data->revision();
    }
    if (selection_.size() == 1) {
        if (const InstancePainter* painter = painter_live_()) {
            if (const InstanceRecord* r = painter->data->at(selection_.refs().front())) {
                single_yaw_deg_ = glm::degrees(instance_yaw(*r));
                single_scale_ = r->scale.x;
            }
        }
    }
}

void InstancePaintPanel::sync_selection_() {
    const InstancePainter* painter = painter_live_();
    if (painter == nullptr || !painter->data) {
        selection_.clear();
        return;
    }
    if (selection_.empty()) {
        selection_revision_ = painter->data->revision();
        return;
    }
    // Anything that added or removed records -- a stroke, an undo, game code -- shifts the
    // indices a ref is, so the selection is dropped rather than left pointing somewhere wrong.
    if (painter->data->revision() != selection_revision_) {
        selection_.clear();
        selection_revision_ = painter->data->revision();
    }
}

void InstancePaintPanel::handle_select_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
    const auto& in = ctx.input();
    const glm::vec2 m = ctx.mouse();
    if (select_drag_.active) {
        select_drag_.to = m;
        if (!in.down[0]) {
            apply_select_drag_(vp);
            select_drag_.active = false;
        }
        return;
    }
    if (hovered && in.pressed[0] && !has(in.mods, Mods::Alt)) {
        select_drag_.active = true;
        select_drag_.add = has(in.mods, Mods::Shift);
        select_drag_.subtract = has(in.mods, Mods::Control) || has(in.mods, Mods::Super);
        select_drag_.from = m;
        select_drag_.to = m;
    }
}

void InstancePaintPanel::edit_selection_(const std::string& label,
                                         const std::function<void(InstanceRecord&)>& fn,
                                         const std::string& merge) {
    InstancePainter* painter = painter_live_();
    if (painter == nullptr || !painter->data || selection_.empty()) return;
    selection_.validate(*painter->data);
    const auto chunks = selection_.chunks();
    InstancePatch before = capture_instance_patch(painter->data, chunks);
    if (selection_.edit(*painter->data, fn) == 0) return;
    InstancePatch after = capture_instance_patch(painter->data, chunks);
    instance_undo_.set_budget(64u << 20, [](const InstancePatch& p) { return p.bytes(); }, 8);
    instance_undo_.push(label, std::move(before), std::move(after), merge);
    // Our own edit: the refs still address the same records, so the selection survives it.
    selection_revision_ = painter->data->revision();
}

void InstancePaintPanel::turn_selection_(float radians) {
    InstancePainter* painter = painter_live_();
    if (painter == nullptr || !painter->data || selection_.empty() || radians == 0.0f) return;
    const glm::vec3 pivot = selection_.centroid(*painter->data);
    edit_selection_("Turn Instances", [&](InstanceRecord& r) { orbit_instance(r, pivot, radians); });
}

void InstancePaintPanel::delete_selection_() {
    InstancePainter* painter = painter_live_();
    if (painter == nullptr || !painter->data || selection_.empty()) return;
    selection_.validate(*painter->data);
    const auto chunks = selection_.chunks();
    InstancePatch before = capture_instance_patch(painter->data, chunks);
    const std::size_t n = selection_.erase(*painter->data);
    if (n == 0) return;
    InstancePatch after = capture_instance_patch(painter->data, chunks);
    instance_undo_.set_budget(64u << 20, [](const InstancePatch& p) { return p.bytes(); }, 8);
    instance_undo_.push("Delete Instances", std::move(before), std::move(after));
    selection_revision_ = painter->data->revision();
    app_.log_info("Deleted " + std::to_string(n) + (n == 1 ? " instance" : " instances"));
}

void InstancePaintPanel::drop_selection_to_surface_() {
    InstancePainter* painter = painter_live_();
    if (painter == nullptr || !painter->data || selection_.empty()) return;
    refresh_surfaces_();
    int dropped = 0;
    edit_selection_("Drop Instances to Surface", [&](InstanceRecord& r) {
        const float from = r.position.z + 50.0f;
        const std::optional<SurfaceHit> hit = drop_onto_surface_(glm::vec2(r.position), from);
        if (!hit) return;
        float sink = 0.0f;
        if (const InstancePrototype* proto = painter->prototype_at(r.prototype)) sink = proto->sink * r.scale.z;
        r.position = hit->point - hit->normal * sink;
        ++dropped;
    });
    app_.log_info("Dropped " + std::to_string(dropped) + (dropped == 1 ? " instance" : " instances") +
                  " onto the surface");
}

void InstancePaintPanel::draw_selection_overlay_(imm::Context& ctx, const ViewProj& vp) {
    const InstancePainter* painter = painter_live_();
    if (painter == nullptr || !painter->data) return;
    const glm::vec4 col = ctx.style.accent;

    // What is selected: a ring at each instance's base and a tick up its height, so a selection
    // reads at a glance without hiding the thing it is on.
    for (const auto& ref : selection_.refs()) {
        const InstanceRecord* r = painter->data->at(ref);
        if (r == nullptr) continue;
        float height = 1.0f;
        float radius = 0.4f;
        if (const InstancePrototype* proto = painter->prototype_at(r->prototype)) {
            height = std::max(0.2f, (proto->bounds_max.z - proto->bounds_min.z) * r->scale.z);
            radius = std::max(0.15f, 0.5f * std::max(proto->bounds_max.x - proto->bounds_min.x,
                                                     proto->bounds_max.y - proto->bounds_min.y) * r->scale.x);
        }
        std::optional<glm::vec2> prev;
        const int n = 16;
        for (int i = 0; i <= n; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
            const glm::vec3 p = r->position + glm::vec3(std::cos(a) * radius, std::sin(a) * radius, 0.05f);
            const auto q = vp.project(p);
            if (prev && q) ctx.line(*prev, *q, col, 1.4f);
            prev = q;
        }
        const auto base = vp.project(r->position + glm::vec3(0.0f, 0.0f, 0.05f));
        const auto top = vp.project(r->position + glm::vec3(0.0f, 0.0f, height));
        if (base && top) ctx.line(*base, *top, imm::with_alpha(col, 0.55f), 1.2f);
    }

    // The rubber band.
    if (select_drag_.active && select_drag_.is_box()) {
        const glm::vec2 lo = glm::min(select_drag_.from, select_drag_.to);
        const glm::vec2 hi = glm::max(select_drag_.from, select_drag_.to);
        const imm::Box box{lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};
        ctx.fill(box, app_.et_.viewport.box_select_fill);
        ctx.outline(box, app_.et_.viewport.box_select_outline);
    }
}

// =================================================================================
// Input
// =================================================================================

void InstancePaintPanel::handle_instance_input_(imm::Context& ctx, const ViewProj& vp, bool hovered) {
    const auto& in = ctx.input();
    const glm::vec2 m = ctx.mouse();
    const bool ctrl = has(in.mods, Mods::Control) || has(in.mods, Mods::Super);
    if (resize_.active) {
        const float dx = m.x - resize_.start.x;
        if (resize_.strength) brush_.density = std::clamp(resize_.start_value * std::exp(dx / 200.0f), 0.001f, 100.0f);
        else brush_.radius = std::clamp(resize_.start_value * std::exp(dx / 200.0f), 0.1f, 200.0f);
        bool cancel = in.pressed[1], confirm = in.pressed[0];
        for (const auto& e : in.keys) {
            if (e.action == coopa::input::KeyAction::Release) continue;
            if (e.key == Key::Escape) cancel = true;
            if (e.key == Key::Enter || e.key == Key::KpEnter) confirm = true;
        }
        ctx.consume_keyboard();
        if (cancel) {
            (resize_.strength ? brush_.density : brush_.radius) = resize_.start_value;
            resize_.active = false;
        } else if (confirm) {
            resize_.active = false;
        }
        return;
    }
    // Select places nothing, so it never starts a stroke: it has its own press/drag/release.
    if (brush_.tool == InstanceTool::Select && !stroke_.active) {
        handle_select_input_(ctx, vp, hovered);
        if (select_drag_.active) return;
        if (!hovered || ctx.wants_keyboard() || ctx.any_popup_open()) return;
        if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete)) delete_selection_();
        if (ctx.shortcut(Key::A, Mods::Alt)) selection_.clear();
    }
    if (stroke_.active) {
        if (!in.down[0]) { instance_end_stroke_(); return; }
        if (in.mouse_delta != glm::vec2(0.0f)) instance_continue_stroke_(vp, m);
        else if ((stroke_.tool == InstanceTool::Paint || stroke_.tool == InstanceTool::Erase ||
                  stroke_.erase) && now_seconds() - g_last_dab_time > k_hold_dab_interval) {
            instance_dab_at_(vp, m);   // holding still keeps painting / erasing
        }
        return;
    }
    if (hovered && in.pressed[0] && !has(in.mods, Mods::Alt) && brush_.tool != InstanceTool::Select) {
        instance_begin_stroke_(vp, m, ctrl);
    }
    if (!hovered || ctx.wants_keyboard() || ctx.any_popup_open()) return;
    // This mode's own keys go FIRST, because a shortcut is consumed by whoever claims it and
    // the viewport keymap binds plain S to the Scale modal. A modal brush owns its letters.
    if (ctx.shortcut(Key::F)) resize_ = {true, false, m, brush_.radius};
    if (ctx.shortcut(Key::F, Mods::Shift)) resize_ = {true, true, m, brush_.density};
    if (ctx.shortcut(Key::LeftBracket)) brush_.radius = std::max(0.1f, brush_.radius / 1.15f);
    if (ctx.shortcut(Key::RightBracket)) brush_.radius = std::min(200.0f, brush_.radius * 1.15f);
    const Key digits[] = {Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5};
    for (int i = 0; i < 5; ++i) {
        if (ctx.shortcut(digits[i])) brush_.tool = static_cast<InstanceTool>(i);
    }
    if (ctx.shortcut(Key::S)) brush_.snap = !brush_.snap;
    if (ctx.shortcut(Key::C, Mods::Control)) convert_nearest_to_object_(vp, m);
    if (ctx.shortcut(Key::Tab) || ctx.shortcut(Key::Escape)) { exit_instance_mode_(); return; }
    // ...then the shared camera / framing keymap, for everything this mode does not claim.
    app_.viewport_keymap_(ctx, vp, false, false);
}

// =================================================================================
// Overlay
// =================================================================================

void InstancePaintPanel::draw_instance_overlay_(imm::Context& ctx, const ViewProj& vp) {
    const InstancePainter* painter = painter_live_();
    if (painter == nullptr) return;
    const bool erasing = stroke_.active ? (stroke_.erase || stroke_.tool == InstanceTool::Erase)
                                        : brush_.tool == InstanceTool::Erase;
    const glm::vec4 col = erasing ? glm::vec4(1.0f, 0.55f, 0.45f, 0.95f) : glm::vec4(1.0f, 1.0f, 1.0f, 0.9f);

    if (brush_.tool == InstanceTool::Select) {
        draw_selection_overlay_(ctx, vp);
        return;
    }
    const glm::vec2 m = resize_.active ? resize_.start : ctx.mouse();
    if (!resize_.active && !(app_.viewport_hovered_ || stroke_.active)) return;
    const std::optional<SurfaceHit> hit = surface_hit_(vp, m);
    if (!hit) return;
    const glm::vec3 centre = hit->point;

    // A ring draped over whatever is under it: each of its points raycast down onto the
    // surface, so the brush reads correctly over a slope, a step or a rock.
    auto ring = [&](float radius, const glm::vec4& c, float width) {
        std::optional<glm::vec2> prev;
        const int n = 48;
        for (int i = 0; i <= n; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
            const glm::vec2 xy = glm::vec2(centre) + glm::vec2(std::cos(a), std::sin(a)) * radius;
            const std::optional<SurfaceHit> h =
                drop_onto_surface_(xy, centre.z + std::max(20.0f, radius * 2.0f));
            const glm::vec3 p = h ? h->point + glm::vec3(0.0f, 0.0f, 0.05f)
                                  : glm::vec3(xy, centre.z + 0.05f);
            const auto q = vp.project(p);
            if (prev && q) ctx.line(*prev, *q, c, width);
            prev = q;
        }
    };

    if (brush_.tool == InstanceTool::Place) {
        // A click tool has no disc: a small cross on the spot it would land.
        const glm::vec2 xy = brush_.snap ? snap_to_grid(brush_, glm::vec2(centre)) : glm::vec2(centre);
        const glm::vec3 p(xy, centre.z + 0.05f);
        if (const auto q = vp.project(p)) {
            ctx.line(*q - glm::vec2(8, 0), *q + glm::vec2(8, 0), col, 1.6f);
            ctx.line(*q - glm::vec2(0, 8), *q + glm::vec2(0, 8), col, 1.6f);
        }
    } else {
        const float r = brush_.tool == InstanceTool::Cluster ? brush_.cluster_spread : brush_.radius;
        ring(r, col, 1.6f);
        if (brush_.tool == InstanceTool::Paint) {
            ring(r * std::max(0.02f, 1.0f - brush_.falloff), imm::with_alpha(col, 0.45f), 1.0f);
        }
    }

    // The snap grid, over the brush only: a full-scene grid would bury the viewport in lines.
    if (brush_.snap && brush_.grid_cell > 0.05f) {
        const float r = brush_.tool == InstanceTool::Cluster ? brush_.cluster_spread : brush_.radius;
        const float cell = brush_.grid_cell;
        const int n = std::min(40, static_cast<int>(std::ceil(2.0f * r / cell)) + 1);
        const glm::vec2 base = snap_to_grid(brush_, glm::vec2(centre) - glm::vec2(r)) - glm::vec2(cell * 0.5f);
        const glm::vec4 grid = imm::with_alpha(col, 0.22f);
        for (int i = 0; i <= n; ++i) {
            const float x = base.x + static_cast<float>(i) * cell;
            const auto a = vp.project(glm::vec3(x, base.y, centre.z + 0.04f));
            const auto b = vp.project(glm::vec3(x, base.y + static_cast<float>(n) * cell, centre.z + 0.04f));
            if (a && b) ctx.line(*a, *b, grid, 1.0f);
            const float y = base.y + static_cast<float>(i) * cell;
            const auto c = vp.project(glm::vec3(base.x, y, centre.z + 0.04f));
            const auto d = vp.project(glm::vec3(base.x + static_cast<float>(n) * cell, y, centre.z + 0.04f));
            if (c && d) ctx.line(*c, *d, grid, 1.0f);
        }
    }

    if (resize_.active) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), resize_.strength ? "Density %.2f /m2" : "Radius %.1f m",
                      resize_.strength ? brush_.density : brush_.radius);
        app_.viewport_chrome_panel_->shadow_text_(ctx, resize_.start + glm::vec2(12.0f, -12.0f), buf,
                                                  ctx.style.text);
    }
}

// =================================================================================
// UI
// =================================================================================

float InstancePaintPanel::draw_instance_header_(imm::Context& ctx, float x, const imm::Box& hb) {
    const float bh = hb.h - 6;
    auto label = [&](const char* t) {
        const float w = ctx.text_width(t) + 6;
        ctx.text_in({x, hb.y, w, hb.h}, t, ctx.style.text_dim, 0.0f);
        x += w;
    };
    label(instance_tool_name(brush_.tool));
    x += 6;
    if (brush_.tool == InstanceTool::Cluster) {
        label("Spread");
        ctx.drag_float_box("ip_spread", {x, hb.y + 3, 66, bh}, &brush_.cluster_spread, 0.05f, 0.1f, 50.0f, "%.1f m");
        ctx.tooltip("Spread\nHow far a clump reaches from the click, in metres");
        x += 72;
    } else if (brush_.tool != InstanceTool::Place) {
        label("Radius");
        ctx.drag_float_box("ip_radius", {x, hb.y + 3, 66, bh}, &brush_.radius, 0.05f, 0.1f, 200.0f, "%.1f m");
        ctx.tooltip("Radius\nBrush radius in metres on the surface (F, [ and ])");
        x += 72;
    }
    if (brush_.tool == InstanceTool::Paint) {
        label("Density");
        ctx.drag_float_box("ip_density", {x, hb.y + 3, 60, bh}, &brush_.density, 0.01f, 0.001f, 100.0f, "%.2f");
        ctx.tooltip("Density\nInstances per square metre at full strength (Shift F)");
        x += 66;
    }
    if (ctx.icon_button("ip_snap", imm::Icon::Grid, "Snap to Grid (S)\nPlace on a grid and quantise yaw", brush_.snap,
                        bh, imm::Context::kAll, imm::Box{x, hb.y + 3, bh, bh})) {
        brush_.snap = !brush_.snap;
    }
    x += bh + 6;
    return x + 8;
}

void InstancePaintPanel::draw_instance_toolbar_(imm::Context& ctx, const imm::Box& r) {
    float y = r.y + 4;
    const float size = app_.viewport_chrome_panel_->kToolSize;
    for (const auto& b : k_tools) {
        const imm::Box bb{r.x + 3, y, size, size};
        y += size + 2;
        if (ctx.icon_button(std::string("instance_") + instance_tool_name(b.tool), b.icon, b.tip,
                            brush_.tool == b.tool, size, imm::Context::kAll, bb)) {
            brush_.tool = b.tool;
        }
    }
}

void InstancePaintPanel::instance_edit_component_(const std::string& label, const std::function<void(Node&)>& fn,
                                                  const std::string& merge) {
    Node comp;
    if (!app_.instances_panel_->shown_component_(instance_object_, "InstancePainter", comp)) return;
    fn(comp);
    app_.instances_panel_->edit_component_(instance_object_, comp, label, merge);
}

void InstancePaintPanel::add_prototype_(const std::string& asset_ref, bool is_object) {
    instance_edit_component_(is_object ? "Add Instance Prototype" : "Add Instance Mesh", [&](Node& c) {
        if (!c.contains("prototypes") || !c.at("prototypes").is_sequence()) c["prototypes"] = Node::sequence();
        Node p = Node::mapping();
        p[is_object ? "object" : "mesh"] = asset_ref;
        c["prototypes"].as_seq().push_back(std::move(p));
    });
    // Select what was just added: the next stroke almost certainly means to paint it.
    if (const InstancePainter* painter = painter_live_()) {
        brush_.prototype = std::max(0, static_cast<int>(painter->prototypes.size()));
    }
}

void InstancePaintPanel::convert_nearest_to_object_(const ViewProj& vp, glm::vec2 px) {
    InstancePainter* painter = painter_live_();
    if (painter == nullptr || !painter->data) return;
    const std::optional<SurfaceHit> hit = surface_hit_(vp, px);
    if (!hit) return;
    std::vector<toy::instance::InstanceRef> near;
    painter->data->query_radius(hit->point, std::max(brush_.radius, 1.0f), near);
    const InstanceRecord* best = nullptr;
    toy::instance::InstanceRef best_ref{};
    float best_d2 = 1e30f;
    for (const auto& ref : near) {
        const InstanceRecord* r = painter->data->at(ref);
        if (r == nullptr) continue;
        const float d2 = glm::dot(r->position - hit->point, r->position - hit->point);
        if (d2 < best_d2) { best_d2 = d2; best = r; best_ref = ref; }
    }
    if (best == nullptr) {
        app_.log_warn("Convert to Object: no painted instance near the cursor");
        return;
    }
    const InstancePrototype* proto = painter->prototype_at(best->prototype);
    if (proto == nullptr || proto->object.empty()) {
        app_.log_warn("Convert to Object: only an `object:` prototype can become a prefab instance");
        return;
    }
    // The record goes first, so the undo step that removes the object does not leave the
    // instance missing as well: the two are separate timelines and this is the honest order.
    const InstanceRecord copy = *best;
    const std::vector<ChunkCoord> touched{best_ref.chunk};
    InstancePatch before = capture_instance_patch(painter->data, touched);
    painter->data->erase(best_ref);
    InstancePatch after = capture_instance_patch(painter->data, touched);
    instance_undo_.push("Convert Instance to Object", std::move(before), std::move(after));

    const ObjectId id = app_.place_object_asset(proto->object, copy.position);
    if (id == 0) return;
    app_.doc_.set_transform(id, copy.position, matrix_to_euler(glm::mat4_cast(copy.rotation)), copy.scale,
                            "Convert Instance to Object");
    app_.log_info("Converted one " + proto->label() + " into an object");
}

EditResult InstancePaintPanel::draw_instance_inspector_header_(imm::Context& ctx, ObjectId id) {
    using I = imm::Icon;
    EditResult r;
    const glm::vec4 dim = ctx.style.text_dim;

    ctx.paragraph("Brush-placed instances of object assets. They are records, not objects, so a "
                  "world of them costs no scene file -- they live in a `.instances` file beside "
                  "the scene, which Ctrl S writes.", &dim);

    coopa::scene::SceneObject* live = app_.sync_.live(id);
    auto* painter = live != nullptr ? live->get_component<InstancePainter>() : nullptr;
    if (painter != nullptr && painter->data) {
        char buf[192];
        std::snprintf(buf, sizeof(buf), "%zu instances in %zu chunks, %zu prototypes",
                      painter->data->size(), painter->data->chunk_count(), painter->prototypes.size());
        ctx.label_dim(buf);
        if (painter->data->unsaved()) ctx.label_dim("Unsaved changes (Ctrl S)");
        if (painter->prototypes.empty()) ctx.label_dim("No prototypes yet -- add one in Paint Instances.");
    }

    const bool can_edit = !app_.playing() && !app_.asset_view_() && is_painter_object_(id);
    if (ctx.button("Paint Instances", -1, can_edit, I::BrushDraw)) enter_instance_mode_(id);
    ctx.tooltip(can_edit ? "Paint instances onto any surface in the scene"
                         : "Stop play to paint instances");
    return r;
}

void InstancePaintPanel::draw_instance_palette_(imm::Context& ctx, InstancePainter& painter) {
    using I = imm::Icon;
    if (!ctx.collapsing_header("Prototypes", true, nullptr, I::Object)) return;
    ctx.label_dim("What the brush places. An object asset brings its own meshes and colliders.");

    for (std::size_t i = 0; i < painter.prototypes.size(); ++i) {
        InstancePrototype& proto = painter.prototypes[i];
        const bool active = brush_.prototype == static_cast<int>(i);
        ctx.push_id("ip_proto" + std::to_string(i));
        std::string title = proto.label();
        if (!proto.resolved) title += "  (loading)";
        else if (proto.parts.empty()) title += "  (nothing to draw)";
        if (ctx.selectable(title, active, 0.0f, active ? I::BrushDraw : I::Object)) {
            brush_.prototype = static_cast<int>(i);
        }
        ctx.tooltip(proto.object.empty() ? ("mesh: " + proto.mesh) : ("object: " + proto.object));
        if (active && ctx.collapsing_header("Placement", false, nullptr, I::Proportional)) {
            auto edit = [&](const std::string& label, const std::function<void(Node&)>& fn,
                            const std::string& merge = {}) {
                instance_edit_component_(label, [&](Node& c) {
                    if (!c.contains("prototypes") || !c.at("prototypes").is_sequence()) return;
                    auto& seq = c["prototypes"].as_seq();
                    if (i < seq.size()) fn(seq[i]);
                }, merge);
            };
            glm::vec2 scale = proto.scale_range;
            if (ctx.drag_floatn("Scale", &scale.x, 2, 0.01f, "%.2f", 0.01f, 100.0f)) {
                edit("Instance Scale Range", [&](Node& n) {
                    Node v = Node::sequence();
                    v.as_seq().push_back(Node(scale.x));
                    v.as_seq().push_back(Node(scale.y));
                    n["scale_range"] = std::move(v);
                }, "ip_scale");
            }
            ctx.tooltip("Scale\nEach instance takes a uniform scale somewhere in this range");
            glm::vec2 yaw = proto.yaw_range;
            if (ctx.drag_floatn("Yaw (deg)", &yaw.x, 2, 1.0f, "%.0f", -360.0f, 360.0f)) {
                edit("Instance Yaw Range", [&](Node& n) {
                    Node v = Node::sequence();
                    v.as_seq().push_back(Node(yaw.x));
                    v.as_seq().push_back(Node(yaw.y));
                    n["yaw_range"] = std::move(v);
                }, "ip_yaw");
            }
            float align = proto.align_to_normal;
            if (ctx.slider_float("Align to Normal", &align, 0.0f, 1.0f, "%.2f")) {
                edit("Instance Align to Normal", [&](Node& n) { n["align_to_normal"] = align; }, "ip_align");
            }
            ctx.tooltip("Align to Normal\n0 stands every instance up; 1 lies it flat on the surface. "
                        "Trees want a little, rocks want a lot.");
            float sink = proto.sink;
            if (ctx.drag_float("Sink (m)", &sink, 0.005f, 0.0f, 5.0f)) {
                edit("Instance Sink", [&](Node& n) { n["sink"] = sink; }, "ip_sink");
            }
            ctx.tooltip("Sink\nMetres pushed into the surface, so a trunk is not perched on a slope");

            const std::vector<std::string> k_collision = {"none", "auto", "box", "sphere", "capsule", "mesh"};
            int ci = 0;
            for (std::size_t k = 0; k < k_collision.size(); ++k) {
                if (k_collision[k] == toy::instance::instance_collision_name(proto.collision)) ci = static_cast<int>(k);
            }
            if (ctx.combo("Collision", &ci, k_collision)) {
                edit("Instance Collision", [&](Node& n) { n["collision"] = k_collision[static_cast<std::size_t>(ci)]; });
            }
            ctx.tooltip("Collision\nColliders exist only within the painter's Collision Radius of the "
                        "camera. `auto` fits a capsule to anything taller than it is wide, else a box.");
            float draw = proto.draw_distance;
            if (ctx.drag_float("Draw Distance (m)", &draw, 1.0f, 0.0f, 20000.0f)) {
                edit("Instance Draw Distance", [&](Node& n) { n["draw_distance"] = draw; }, "ip_draw");
            }
            ctx.tooltip("Draw Distance\n0 derives one from how tall the prototype is");
            bool remove = false;
            if (ctx.button("Remove Prototype", -1, true, I::Trash)) remove = true;
            ctx.pop_id();
            if (remove) {
                instance_edit_component_("Remove Instance Prototype", [&](Node& c) {
                    if (!c.contains("prototypes") || !c.at("prototypes").is_sequence()) return;
                    auto& seq = c["prototypes"].as_seq();
                    if (i < seq.size()) seq.erase(seq.begin() + static_cast<std::ptrdiff_t>(i));
                });
                brush_.prototype = 0;
                return;
            }
            continue;
        }
        ctx.pop_id();
    }

    // Adding one: the project's object assets, then its meshes.
    std::vector<std::string> objects = app_.inspector_env_().list_assets("objects", ".yaml");
    if (!objects.empty()) {
        std::vector<std::string> shown{"Add Object Asset..."};
        for (const std::string& o : objects) shown.push_back(o);
        int pick = 0;
        if (ctx.combo("Add", &pick, shown) && pick > 0) {
            add_prototype_(objects[static_cast<std::size_t>(pick - 1)], true);
        }
    }
    std::vector<std::string> meshes = app_.inspector_env_().list_assets("meshes", ".yaml");
    if (!meshes.empty()) {
        std::vector<std::string> shown{"Add Mesh..."};
        for (const std::string& m : meshes) shown.push_back(m);
        int pick = 0;
        if (ctx.combo("Add Mesh", &pick, shown) && pick > 0) {
            add_prototype_(meshes[static_cast<std::size_t>(pick - 1)], false);
        }
    }
}

void InstancePaintPanel::draw_selection_panel_(imm::Context& ctx, InstancePainter& painter) {
    using I = imm::Icon;
    if (!ctx.collapsing_header("Selection", true, nullptr, I::SelectBox)) return;
    selection_.validate(*painter.data);

    if (selection_.empty()) {
        ctx.label_dim("Click an instance, or drag a box over several. Shift adds, Ctrl removes.");
        return;
    }
    const bool one = selection_.size() == 1;
    char buf[96];
    std::snprintf(buf, sizeof(buf), one ? "%zu instance selected" : "%zu instances selected",
                  selection_.size());
    ctx.label(buf);

    // --- rotation ---
    if (one) {
        // One instance has a heading worth showing, so it is set outright.
        if (ctx.drag_float("Yaw (deg)", &single_yaw_deg_, 1.0f, -3600.0f, 3600.0f)) {
            const float radians = glm::radians(single_yaw_deg_);
            edit_selection_("Set Instance Yaw", [&](InstanceRecord& r) { set_instance_yaw(r, radians); },
                            "sel_yaw");
        }
        ctx.tooltip("Yaw\nWhich way this instance faces, degrees about the world's up axis");
    } else {
        // A group has no single heading: the control TURNS them instead, which is the thing
        // you actually want -- and about their shared centre, so the arrangement holds.
        float delta = 0.0f;
        if (ctx.drag_float("Turn (deg)", &group_turn_deg_, 1.0f, -3600.0f, 3600.0f)) {
            delta = glm::radians(group_turn_deg_);
            group_turn_deg_ = 0.0f;   // a relative control springs back to zero
        }
        if (delta != 0.0f) turn_selection_(delta);
        ctx.tooltip("Turn\nSwings the whole selection about its centre, so the arrangement holds "
                    "instead of each instance spinning where it stands");
        if (ctx.button("Spin Each 90", -1, true, I::Rotate)) {
            edit_selection_("Spin Instances", [](InstanceRecord& r) { turn_instance(r, glm::half_pi<float>()); });
        }
        ctx.tooltip("Spin Each 90\nTurns every selected instance in place, a quarter turn");
        if (ctx.button("Randomize Yaw", -1, true, I::Dots)) {
            edit_selection_("Randomize Instance Yaw", [](InstanceRecord& r) {
                // From the record's own seed, so the same selection always lands the same way.
                const float t = static_cast<float>(r.seed % 36000u) / 36000.0f;
                set_instance_yaw(r, t * 6.2831853f);
            });
        }
    }

    // --- scale ---
    if (one) {
        if (ctx.drag_float("Scale", &single_scale_, 0.01f, 0.01f, 100.0f)) {
            const float v = single_scale_;
            edit_selection_("Set Instance Scale", [&](InstanceRecord& r) { set_instance_scale(r, v); },
                            "sel_scale");
        }
    } else {
        if (ctx.button("Scale x1.1", -1, true, I::Scale)) {
            edit_selection_("Scale Instances", [](InstanceRecord& r) { scale_instance(r, 1.1f); });
        }
        if (ctx.button("Scale x0.9", -1, true, I::Scale)) {
            edit_selection_("Scale Instances", [](InstanceRecord& r) { scale_instance(r, 1.0f / 1.1f); });
        }
    }

    // --- height ---
    if (ctx.drag_float("Raise (m)", &group_raise_m_, 0.01f, -100.0f, 100.0f)) {
        const float dz = group_raise_m_;
        group_raise_m_ = 0.0f;
        edit_selection_("Raise Instances", [&](InstanceRecord& r) { offset_instance(r, glm::vec3(0.0f, 0.0f, dz)); });
    }
    ctx.tooltip("Raise\nMoves the selection up or down. Springs back to zero: it is a nudge, not a height");

    if (ctx.button("Drop to Surface", -1, true, I::Terrain)) drop_selection_to_surface_();
    ctx.tooltip("Drop to Surface\nRe-places each selected instance on whatever is under it now -- "
                "what you want after sculpting the ground they were standing on");

    // --- prototype ---
    if (painter.prototypes.size() > 1) {
        std::vector<std::string> names;
        for (const auto& proto : painter.prototypes) names.push_back(proto.label());
        int pick = std::clamp(brush_.prototype, 0, static_cast<int>(names.size()) - 1);
        if (ctx.combo("Change To", &pick, names)) {
            const auto index = static_cast<std::uint16_t>(pick);
            edit_selection_("Change Instance Prototype", [&](InstanceRecord& r) { r.prototype = index; });
            brush_.prototype = pick;
        }
        ctx.tooltip("Change To\nSwaps the selected instances for another prototype, where they stand");
    }

    if (ctx.button("Delete Selected", -1, true, I::Trash)) delete_selection_();
    ctx.tooltip("Delete Selected (X)");
    if (ctx.button("Clear Selection", -1, true, I::X)) selection_.clear();
    ctx.tooltip("Clear Selection (Alt A)");
}

void InstancePaintPanel::draw_instance_tool_panel_(imm::Context& ctx) {
    using I = imm::Icon;
    InstancePainter* painter = painter_live_();
    const Node* obj = app_.doc_.find(instance_object_);
    ctx.label(std::string("Painter: ") + (obj != nullptr ? get_string(*obj, "name", "?") : "?"));
    if (ctx.button("Exit Instance Paint Mode", -1, true, I::ObjectMode)) { exit_instance_mode_(); return; }
    ctx.tooltip("Exit Instance Paint Mode (Tab)");
    if (painter == nullptr || !painter->data) {
        ctx.label_dim("The painter is still loading.");
        return;
    }

    if (ctx.collapsing_header("Tools", true, nullptr, I::BrushDraw)) {
        for (const auto& b : k_tools) {
            if (ctx.selectable(instance_tool_name(b.tool), brush_.tool == b.tool, 0.0f, b.icon)) brush_.tool = b.tool;
            ctx.tooltip(b.tip);
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%zu instances painted", painter->data->size());
        ctx.label_dim(buf);
    }

    if (brush_.tool == InstanceTool::Select) {
        draw_selection_panel_(ctx, *painter);
        draw_instance_palette_(ctx, *painter);
        return;
    }

    if (ctx.collapsing_header("Brush", true, nullptr, I::Proportional)) {
        if (brush_.tool == InstanceTool::Cluster) {
            ctx.drag_int("Count Min", &brush_.cluster_count.x, 0.2f, 1, 200);
            ctx.drag_int("Count Max", &brush_.cluster_count.y, 0.2f, 1, 200);
            ctx.drag_float("Spread (m)", &brush_.cluster_spread, 0.05f, 0.1f, 50.0f);
            ctx.slider_float("Centre Bias", &brush_.cluster_bias, 0.0f, 1.0f, "%.2f");
            ctx.tooltip("Centre Bias\n0 spreads the clump evenly; 1 crowds it at the middle");
            ctx.checkbox("Mix Prototypes", &brush_.cluster_mixed);
            if (brush_.cluster_mixed) {
                ctx.drag_int("Mix First N", &brush_.cluster_prototypes, 0.2f, 1,
                             std::max(1, static_cast<int>(painter->prototypes.size())));
                ctx.tooltip("Mix First N\nA clump draws at random from the first N prototypes");
            }
        } else {
            ctx.drag_float("Radius (m)", &brush_.radius, 0.05f, 0.1f, 200.0f);
            ctx.tooltip("Radius\nIn metres on the surface. F drags it; [ and ] step it.");
            if (brush_.tool == InstanceTool::Paint) {
                ctx.drag_float("Density (/m2)", &brush_.density, 0.01f, 0.001f, 100.0f);
                ctx.tooltip("Density\nInstances per square metre at full strength (Shift F)");
                ctx.slider_float("Falloff", &brush_.falloff, 0.0f, 1.0f, "%.2f");
                ctx.tooltip("Falloff\n0: a hard-edged disc. 1: thinning all the way in. The inner "
                            "ring shows the fully dense core.");
                ctx.slider_float("Spacing", &brush_.spacing, 0.05f, 1.0f, "%.2f");
                ctx.tooltip("Spacing\nDistance between dabs along a stroke, as a fraction of the radius");
            }
        }
        if (brush_.tool != InstanceTool::Erase) {
            ctx.drag_float("Min Spacing (m)", &brush_.min_spacing, 0.01f, 0.0f, 50.0f);
            ctx.tooltip("Min Spacing\nHow far an instance keeps clear of every existing one. Without "
                        "it a slow drag piles everything on one spot.");
        } else {
            ctx.checkbox("Erase Active Only", &brush_.erase_active_only);
            ctx.tooltip("Erase Active Only\nRemove only the selected prototype, leaving the rest");
        }
    }

    if (ctx.collapsing_header("Snap", false, nullptr, I::Grid, &brush_.snap)) {
        ctx.drag_float("Cell (m)", &brush_.grid_cell, 0.01f, 0.01f, 100.0f);
        ctx.drag_floatn("Origin", &brush_.grid_origin.x, 2, 0.01f, "%.2f");
        ctx.tooltip("Origin\nThe grid's phase, so a kit can line up with something already placed");
        ctx.drag_float("Yaw Step (deg)", &brush_.yaw_step, 1.0f, 0.0f, 180.0f);
        ctx.tooltip("Yaw Step\nRotation is quantised to this. 90 for walls, 0 to leave yaw alone.");
        ctx.checkbox("One Per Cell", &brush_.one_per_cell);
    }

    draw_instance_palette_(ctx, *painter);

    if (ctx.collapsing_header("Painter", false, nullptr, I::World)) {
        float chunk = painter->chunk_size;
        if (ctx.drag_float("Chunk Size (m)", &chunk, 0.5f, 4.0f, 512.0f)) {
            instance_edit_component_("Instance Chunk Size", [&](Node& c) { c["chunk_size"] = chunk; }, "ip_chunk");
        }
        ctx.tooltip("Chunk Size\nThe draw, cull, collide and file unit. Match the terrain's chunks.");
        float radius = painter->collision_radius;
        if (ctx.drag_float("Collision Radius (m)", &radius, 1.0f, 0.0f, 4000.0f)) {
            instance_edit_component_("Instance Collision Radius", [&](Node& c) { c["collision_radius"] = radius; },
                                     "ip_crad");
        }
        ctx.tooltip("Collision Radius\nColliders exist only this far from the camera. 0 turns "
                    "collision off entirely, whatever the prototypes ask for.");
        bool shadows = painter->cast_shadows;
        if (ctx.checkbox("Cast Shadows", &shadows)) {
            instance_edit_component_("Instance Cast Shadows", [&](Node& c) { c["cast_shadows"] = shadows; });
        }
        if (ctx.button("Save Instances", -1, painter->data->unsaved(), I::Save)) save_instances_();
        ctx.tooltip("Written with the scene by Ctrl S too");
    }
}

} // namespace editor
} // namespace toy
