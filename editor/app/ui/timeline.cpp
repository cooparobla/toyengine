#include "editor/app/ui/timeline.h"
#include "editor/app/ui/instances.h"
#include "editor/app/ui/assets.h"

#include "editor/app/editor_app.h"

#include <coopa/animation/animation_yaml.h>   // parse_clip

namespace toy {
namespace editor {

bool TimelinePanel::in_anim_rig_(ObjectId id) const {
    return app_.anim_rig_ && (id == app_.anim_rig_ || app_.doc_.is_ancestor(app_.anim_rig_, id));
}

fs::path TimelinePanel::anim_scene_dir_() const {
    return (app_.doc_.path().empty() ? app_.sync_.fallback_path : app_.doc_.path()).parent_path();
}

fs::path TimelinePanel::anim_new_clip_base_() const {
    return app_.doc_.is_object_asset() ? app_.project_.assets() : anim_scene_dir_();
}

bool TimelinePanel::anim_live_edit_(ObjectId id) const {
    return (app_.in_pose_mode_() || app_.anim_record_) && app_.anim_posed_ && in_anim_rig_(id);
}

template<typename XOf, typename TOf>
    void TimelinePanel::draw_events_lane_(imm::Context& ctx, const imm::Box& track, float y, float row_h, const XOf& x_of, const TOf& t_of,
                           float pps, bool in_track, bool& on_key) {
    const auto& in = ctx.input();
        const glm::vec2 m = ctx.mouse();
        const auto& evs = app_.anim_clip_.model.events;
        if (app_.anim_sel_event_ >= static_cast<int>(evs.size())) app_.anim_sel_event_ = -1;
        ctx.push_clip(track);
        for (size_t i = 0; i < evs.size(); ++i) {
            const bool sel = static_cast<int>(i) == app_.anim_sel_event_;
            const float ex = x_of(evs[i].time) + (app_.anim_event_drag_.active && sel ? app_.anim_event_drag_.dt * pps : 0.0f);
            const glm::vec4 col = sel ? glm::vec4(1.0f, 0.62f, 0.18f, 1) : glm::vec4(0.55f, 0.8f, 0.95f, 1);
            ctx.line({ex, y + 3}, {ex, y + row_h - 3}, col, 1.5f);
            ctx.triangle({ex, y + 3}, {ex + 9, y + 6.5f}, {ex, y + 10}, col);
            const imm::Box hit{ex - 4, y, 15, row_h};
            if (!ctx.is_hovered(hit) || on_key) continue;
            ctx.text_in({ex + 11, y, 160, row_h}, evs[i].name + (evs[i].string_value.empty() ? "" : " (" + evs[i].string_value + ")"),
                        ctx.style.text, 0.0f);
            if (in.pressed[0]) {
                on_key = true;
                app_.anim_sel_event_ = static_cast<int>(i);
                app_.anim_sel_keys_.clear();
                app_.anim_event_drag_ = {true, m.x, 0.0f};
            } else if (in.pressed[1]) {
                on_key = true;
                app_.anim_sel_event_ = static_cast<int>(i);
                ctx.open_popup("event_menu", m);
            }
        }
        ctx.pop_clip();
        const imm::Box lane{track.x, y, track.w, row_h};
        if (in_track && !on_key && ctx.is_hovered(lane)) {
            if (in.pressed[0]) { app_.anim_sel_event_ = -1; app_.anim_sel_keys_.clear(); on_key = true; }   // empty lane: deselect
            if (in.pressed[1]) {
                on_key = true;
                app_.anim_menu_time_ = anim_snap_(t_of(m.x));
                ctx.open_popup("events_lane_menu", m);
            }
        }
}

ObjectId TimelinePanel::animation_rig() const {
    return app_.anim_rig_;
}

const ClipModel* TimelinePanel::animation_clip() const {
    return app_.anim_clip_.open() ? &app_.anim_clip_.model : nullptr;
}

std::string TimelinePanel::animation_clip_state() const {
    return app_.anim_clip_.state;
}

float TimelinePanel::animation_time() const {
    return app_.anim_time_;
}

void TimelinePanel::set_animation_time(float t) {
    app_.anim_time_ = std::max(0.0f, t);
}

void TimelinePanel::set_animation_record(bool on) {
    app_.anim_record_ = on;
}

bool TimelinePanel::animation_record() const {
    return app_.anim_record_;
}

void TimelinePanel::set_timeline_rest_pose(bool on) {
    app_.anim_show_rest_ = on;
}

std::vector<std::string> TimelinePanel::list_assets(AssetType t) {
    return app_.assets_panel_->list_assets_(t);
}

int TimelinePanel::selected_animation_event() const {
    return app_.anim_sel_event_;
}

void TimelinePanel::select_animation_event(int index) {
    app_.anim_sel_event_ = index;
}

ObjectId TimelinePanel::rig_of_(ObjectId id) const {
    // The EFFECTIVE node, not the document's own: an object asset whose root inherits another
    // asset's Animator is a rig too, and its clips are the ones it should open.
    for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = app_.doc_.parent_of(*cur)) {
        if (anim_animator_of_(*cur)) return *cur;
    }
    return 0;
}

std::string TimelinePanel::rig_path_of_(ObjectId rig, ObjectId id) const {
    std::vector<std::string> names;
    for (std::optional<ObjectId> cur = id; cur && *cur != 0 && *cur != rig; cur = app_.doc_.parent_of(*cur)) {
        const Node* n = app_.doc_.find(*cur);
        names.push_back(n ? get_string(*n, "name") : std::string());
    }
    std::string out;
    for (auto it = names.rbegin(); it != names.rend(); ++it) out += (out.empty() ? "" : "/") + *it;
    return out;
}

std::vector<std::pair<ObjectId, int>> TimelinePanel::rig_objects_(ObjectId rig) const {
    std::vector<std::pair<ObjectId, int>> out;
    if (!rig) return out;
    app_.doc_.for_each_object([&](const Node& n, int depth) {
        const ObjectId id = SceneDocument::id_of(n);
        if (id == rig || app_.doc_.is_ancestor(rig, id)) out.push_back({id, depth});
    });
    if (!out.empty()) {
        const int base = out.front().second;
        for (auto& [id, d] : out) d -= base;
    }
    return out;
}

fs::path TimelinePanel::anim_resolve_clip_(const std::string& rel) const {
    const fs::path local = anim_scene_dir_() / rel;
    if (coopa::yaml::document_exists(local)) return local;
    const fs::path shared = app_.project_.assets() / rel;
    if (coopa::yaml::document_exists(shared)) return shared;
    return anim_new_clip_base_() / rel;
}

std::string TimelinePanel::anim_name_of_(ObjectId id, const std::string& fallback) const {
    const Node* n = app_.doc_.find(id);
    return n ? get_string(*n, "name", fallback) : fallback;
}

std::string TimelinePanel::anim_instance_asset_(ObjectId id) const {
    for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = app_.doc_.parent_of(*cur)) {
        const Node* n = app_.doc_.find(*cur);
        if (!n) break;
        const std::string pf = get_string(*n, "prefab", get_string(*n, "inherit_from"));
        if (!pf.empty()) return pf;
    }
    return {};
}

const Node* TimelinePanel::anim_effective_node_(ObjectId id) const {
    // Resolving an inherited Animator applies to the object asset BEING EDITED — an asset whose
    // root starts from another asset still owns its clip list and must open a dope sheet. A
    // placed copy in a scene is the opposite case: its rig and clips belong to the asset, and
    // draw_timeline_() sends the user there instead, so it must keep reading its own node.
    if (app_.doc_.is_object_asset() && app_.doc_.object_root() != 0 &&
        app_.doc_.outermost_instance_of(id) == app_.doc_.object_root()) {
        if (const Node* r = app_.instances_panel_->resolved_of_(id)) return r;
    }
    return app_.doc_.find(id);
}

const Node* TimelinePanel::anim_animator_of_(ObjectId id) const {
    const Node* n = anim_effective_node_(id);
    if (!n || !n->contains("components") || !n->at("components").is_sequence()) return nullptr;
    for (const Node& c : n->at("components").as_seq()) {
        if (component_type(c) == "Animator") return &c;
    }
    return nullptr;
}

/** @brief The `states` sequence of the Animator in a components list, or an empty sequence. */
static Node states_in_components(const Node* obj) {
    if (!obj || !obj->contains("components") || !obj->at("components").is_sequence()) return Node::sequence();
    for (const Node& c : obj->at("components").as_seq()) {
        if (component_type(c) != "Animator") continue;
        if (c.contains("states") && c.at("states").is_sequence()) return c.at("states");
        return Node::sequence();
    }
    return Node::sequence();
}

static bool states_contain(const Node& states, const std::string& name) {
    if (!states.is_sequence()) return false;
    for (const Node& s : states.as_seq()) if (get_string(s, "name") == name) return true;
    return false;
}

Node TimelinePanel::anim_own_states_() const {
    return app_.anim_rig_ ? states_in_components(app_.doc_.find(app_.anim_rig_)) : Node::sequence();
}

Node TimelinePanel::anim_asset_states_() const {
    return app_.anim_rig_ ? states_in_components(app_.instances_panel_->base_of_(app_.anim_rig_)) : Node::sequence();
}

bool TimelinePanel::anim_state_is_inherited_(const std::string& name) const {
    if (!app_.anim_rig_ || !app_.instances_panel_->in_instance_(app_.anim_rig_)) return false;
    return states_contain(anim_asset_states_(), name) && !states_contain(anim_own_states_(), name);
}

std::vector<TimelinePanel::AnimStateRef> TimelinePanel::anim_states_() const {
    std::vector<AnimStateRef> out;
    if (!app_.anim_rig_) return out;
    const Node* anim = anim_animator_of_(app_.anim_rig_);
    if (!anim || !anim->contains("states") || !anim->at("states").is_sequence()) return out;

    // Classifying costs two sequence scans per state, so only do it for a rig that inherits.
    const bool inheriting = app_.instances_panel_->in_instance_(app_.anim_rig_);
    const Node own = inheriting ? anim_own_states_() : Node::sequence();
    const Node from_asset = inheriting ? anim_asset_states_() : Node::sequence();

    for (const Node& s : anim->at("states").as_seq()) {
        AnimStateRef r{get_string(s, "name"), get_string(s, "clip"), AnimStateOrigin::Own};
        if (inheriting && states_contain(from_asset, r.name)) {
            r.origin = states_contain(own, r.name) ? AnimStateOrigin::Overridden : AnimStateOrigin::Inherited;
        }
        out.push_back(std::move(r));
    }
    return out;
}

void TimelinePanel::anim_edit_animator_(const std::function<void(Node&)>& fn, const std::string& label) {
    if (!app_.anim_rig_) return;
    const int ci = app_.doc_.find_component(app_.anim_rig_, "Animator");
    if (ci >= 0) {
        Node comp = app_.doc_.find(app_.anim_rig_)->at("components").as_seq()[static_cast<size_t>(ci)];
        fn(comp);
        app_.apply_(app_.doc_.set_component(app_.anim_rig_, ci, comp, label));
        return;
    }
    // The rig inherits its Animator: carry the change as an override component holding only the
    // states this variant restates. SceneInheritance merges `states` by name, so the asset—s
    // other clips keep arriving on their own.
    Node comp = Node::mapping();
    comp["type"] = Node(std::string("Animator"));
    comp["states"] = Node::sequence();
    fn(comp);
    app_.apply_(app_.doc_.add_component(app_.anim_rig_, comp));
}

void TimelinePanel::anim_open_clip_(const std::string& name) {
    for (const AnimStateRef& s : anim_states_()) {
        if (s.name != name) continue;
        const fs::path p = anim_resolve_clip_(s.clip);
        app_.anim_clip_ = ClipDocument{};
        app_.anim_clip_.state = s.name;
        app_.anim_clip_.path = p;
        try {
            if (coopa::yaml::document_exists(p)) app_.anim_clip_.model = ClipModel::from_node(coopa::yaml::load_document(p));
            else app_.anim_clip_.model.name = s.name;
        } catch (const std::exception& e) {
            app_.log_error("Timeline: " + std::string(e.what()));
        }
        app_.anim_sel_keys_.clear();
        app_.anim_runtime_rev_ = 0;
        app_.anim_rename_buf_.clear();
        app_.anim_view_ = {};
        return;
    }
}

void TimelinePanel::anim_clip_edit_(const std::string& label, const std::function<void(ClipModel&)>& fn, const std::string& merge_key) {
    if (!app_.anim_clip_.open()) return;
    ClipModel before = app_.anim_clip_.model;
    fn(app_.anim_clip_.model);
    if (app_.anim_clip_.model == before) return;
    app_.anim_clip_.undo.push(label, std::move(before), app_.anim_clip_.model, merge_key);
    ++app_.anim_clip_.revision;
    app_.anim_clip_dirty_ = true;
}

void TimelinePanel::anim_save_clip_() {
    app_.anim_clip_dirty_ = false;
    try {
        fs::create_directories(app_.anim_clip_.path.parent_path());
        coopa::yaml::save_document(app_.anim_clip_.path, app_.anim_clip_.model.to_node());
    } catch (const std::exception& e) {
        app_.log_error("Timeline: could not save " + app_.anim_clip_.path.string() + ": " + e.what());
    }
    ++app_.anim_clip_.revision;
}

void TimelinePanel::anim_add_animator_(ObjectId id) {
    Node comp = Node::mapping();
    comp["type"] = Node(std::string("Animator"));
    comp["auto_play"] = Node(std::string(""));
    comp["states"] = Node::sequence();
    app_.apply_(app_.doc_.add_component(id, comp));
    app_.anim_rig_ = id;
}

std::string TimelinePanel::anim_clip_dir_() const {
    // Where this rig already keeps its clips, so a new one joins them instead of starting a
    // second folder. Object names and clip folders are unrelated: a whole family of rigs can
    // share one clip set (every soft_<race> plays animations/soft_hero/*), and a new clip
    // belongs beside the rest of them.
    std::string dir;
    for (const AnimStateRef& st : anim_states_()) {
        if (st.clip.empty()) continue;
        const std::string d = fs::path(st.clip).parent_path().generic_string();
        if (d.empty()) continue;
        if (dir.empty()) dir = d;
        else if (dir != d) return "animations/" + anim_name_of_(app_.anim_rig_, "Rig");   // already split
    }
    return dir.empty() ? "animations/" + anim_name_of_(app_.anim_rig_, "Rig") : dir;
}

void TimelinePanel::anim_new_clip_(std::string name) {
    if (!app_.anim_rig_) return;
    const auto states = anim_states_();
    auto taken = [&](const std::string& n) {
        for (const auto& s : states) if (s.name == n) return true;
        return false;
    };
    const std::string base = name;
    for (int i = 1; taken(name); ++i) {   // clip, clip_001, clip_002... (snake_case, like assets/)
        char buf[16];
        std::snprintf(buf, sizeof(buf), "_%03d", i);
        name = base + buf;
    }
    const std::string rel = anim_clip_dir_() + "/" + name + ".yaml";
    ClipModel m;
    m.name = name;
    try {
        fs::create_directories((anim_new_clip_base_() / rel).parent_path());
        coopa::yaml::save_document(anim_new_clip_base_() / rel, m.to_node());
    } catch (const std::exception& e) {
        app_.log_error("Timeline: " + std::string(e.what()));
        return;
    }
    const bool first = states.empty();
    anim_edit_animator_([&](Node& comp) {
        if (!comp.contains("states") || !comp.at("states").is_sequence()) comp["states"] = Node::sequence();
        Node st = Node::mapping();
        st["name"] = Node(name);
        st["clip"] = Node(rel);
        comp["states"].as_seq().push_back(st);
        if (first && get_string(comp, "auto_play").empty()) comp["auto_play"] = Node(name);
    }, "New Clip");
    anim_open_clip_(name);
    app_.anim_time_ = 0.0f;
    app_.log_info("Timeline: new clip " + rel);
}

void TimelinePanel::anim_delete_clip_(bool keep_file) {
    if (!app_.anim_rig_ || !app_.anim_clip_.open()) return;
    const std::string state = app_.anim_clip_.state;
    const fs::path path = app_.anim_clip_.path;
    const bool inherited = anim_state_is_inherited_(state);

    // The state that takes over `auto_play`: the first one left after this is gone.
    std::string next_auto;
    for (const AnimStateRef& st : anim_states_()) {
        if (st.name != state) { next_auto = st.name; break; }
    }

    anim_edit_animator_([&](Node& comp) {
        Node kept = Node::sequence();
        if (comp.contains("states") && comp.at("states").is_sequence()) {
            for (const Node& st : comp.at("states").as_seq()) {
                if (get_string(st, "name") != state) kept.as_seq().push_back(st);
            }
        }
        if (inherited) {
            // The object asset declares it, so dropping it here is an override, not a deletion.
            Node gone = Node::mapping();
            gone["name"] = Node(state);
            gone["remove"] = Node(true);
            kept.as_seq().push_back(gone);
        }
        comp["states"] = kept;
        if (get_string(comp, "auto_play") == state) comp["auto_play"] = Node(next_auto);
    }, "Delete Clip");

    // An inherited state—s clip file still belongs to the object asset that declares it.
    if (inherited) {
        app_.log_info("Timeline: " + state + " removed from this rig (its clip stays with the object asset)");
    } else if (keep_file) {
        app_.log_info("Timeline: removed the state " + state + " (its clip file is still used elsewhere)");
    } else {
        std::error_code ec;
        fs::remove(path, ec);
        app_.log_info("Timeline: deleted clip " + state);
    }
    app_.anim_clip_ = ClipDocument{};
    app_.anim_sel_keys_.clear();
}

bool TimelinePanel::get_object_transform_(ObjectId id, glm::vec3& p, glm::vec3& r, glm::vec3& s) const {
    if (anim_live_edit_(id)) {
        if (auto* live = app_.sync_.live(id); live && live->get_transform()) {
            const auto& t = live->get_transform()->transform();
            p = t.position();
            r = t.rotation_degrees();
            s = t.scale();
            return true;
        }
    }
    // An inherited child's document node holds only its overrides: show the merged pose.
    if (app_.doc_.is_inherited(id)) return app_.instances_panel_->instance_transform_(id, p, r, s);
    return app_.doc_.get_transform(id, p, r, s);
}

void TimelinePanel::set_object_transform_(ObjectId id, const glm::vec3& p, const glm::vec3& r, const glm::vec3& s, const std::string& label,
                           const std::string& merge_key) {
    if (anim_live_edit_(id)) {
        if (auto* live = app_.sync_.live(id); live && live->get_transform()) {
            auto& t = live->get_transform()->transform();
            const glm::vec3 op = t.position(), orr = t.rotation_degrees(), os = t.scale();
            t.set_position(p);
            t.set_rotation(r);
            t.set_scale(s);
            // Auto-key: the channels this edit changed, at the playhead.
            const bool dp = glm::distance(op, p) > 1e-6f, dr = glm::distance(orr, r) > 1e-4f, ds = glm::distance(os, s) > 1e-6f;
            // Auto-Key: outside Pose mode only Record reaches here, so this reads the same.
            if ((dp || dr || ds) && app_.anim_clip_.open() && app_.anim_record_) {
                const std::string path = rig_path_of_(app_.anim_rig_, id);
                const glm::quat q = t.rotation_quat();
                const float time = app_.anim_time_;
                anim_clip_edit_("Auto Key", [&](ClipModel& m) {
                    if (dp) m.set_key(path, "position", time, glm::vec4(p, 0.0f));
                    if (dr) m.set_key(path, "rotation_quat", time, glm::vec4(q.x, q.y, q.z, q.w));
                    if (ds) m.set_key(path, "scale", time, glm::vec4(s, 0.0f));
                    if (time > m.length) m.length = time;
                }, "autokey");
                app_.anim_pose_rev_ = app_.anim_clip_.revision;   // this pose IS the key: no re-pose needed
                app_.anim_runtime_rev_ = 0;                  // ...but the sampler must see the new keys later
            }
        }
        return;
    }
    if (app_.doc_.is_inherited(id)) { app_.instances_panel_->set_inherited_transform_(id, p, r, s, label, merge_key); return; }
    app_.apply_(app_.doc_.set_transform(id, p, r, s, label, merge_key));
}

void TimelinePanel::anim_restore_rest_pose_() {
    for (const auto& [id, depth] : rig_objects_(app_.anim_rig_)) {
        glm::vec3 p, r, s;
        auto* live = app_.sync_.live(id);
        if (!live || !live->get_transform() || !app_.doc_.get_transform(id, p, r, s)) continue;
        auto& t = live->get_transform()->transform();
        t.set_position(p);
        t.set_rotation(r);
        t.set_scale(s);
    }
}

bool TimelinePanel::anim_preview_wanted_() const {
    return app_.show_bottom_ && app_.bottom_view_ == 1 && app_.anim_rig_ && app_.anim_clip_.open() && !app_.anim_show_rest_ && !app_.playing() &&
           (app_.active_type_ == AssetType::Scene || app_.active_type_ == AssetType::Object) && app_.sync_.live(app_.anim_rig_) != nullptr;
}

void TimelinePanel::timeline_frame_(float dt) {
    // Clip edits are written once a frame; an auto-key drag is one undo step until the mouse lets go.
    if (app_.anim_clip_dirty_ && app_.anim_clip_.open()) { anim_save_clip_(); app_.anim_clip_dirty_ = false; }
    if (!app_.ui().input().down[0]) app_.anim_clip_.undo.end_merge();
    // Another document (scene / object asset): ids restart there, so nothing carries over.
    const fs::path doc_path = app_.doc_.path();
    if (doc_path != app_.anim_doc_path_) {
        app_.anim_doc_path_ = doc_path;
        app_.anim_rig_ = 0;
        app_.anim_posed_ = false;   // the live objects it posed belong to the old document
        app_.anim_record_ = false;
        app_.anim_playing_ = false;
        app_.anim_time_ = 0.0f;     // the playhead too: a new document's rig opens at its rest frame
        app_.anim_clip_ = ClipDocument{};
        app_.anim_sel_keys_.clear();
    }
    // The rig: the selection's, kept while the selection has none (clicking around the scene).
    if (app_.anim_rig_ && !app_.doc_.find(app_.anim_rig_)) { app_.anim_rig_ = 0; app_.anim_posed_ = false; app_.anim_clip_ = ClipDocument{}; }
    if (const ObjectId sel = app_.doc_.primary()) {
        const ObjectId r = rig_of_(sel);
        if (r && r != app_.anim_rig_) {
            if (app_.anim_posed_) { anim_restore_rest_pose_(); app_.anim_posed_ = false; }
            app_.anim_rig_ = r;
            app_.anim_clip_ = ClipDocument{};
            app_.anim_record_ = false;
        }
    }
    if (app_.anim_rig_ && !anim_animator_of_(app_.anim_rig_)) {
        if (app_.anim_posed_) { anim_restore_rest_pose_(); app_.anim_posed_ = false; }
        app_.anim_clip_ = ClipDocument{};
    }
    // The open clip must still be one of the rig's states.
    if (app_.anim_clip_.open()) {
        bool still = false;
        for (const auto& s : anim_states_()) still |= s.name == app_.anim_clip_.state;
        if (!still) app_.anim_clip_ = ClipDocument{};
    }
    if (!app_.anim_clip_.open() && app_.anim_rig_) {
        const auto states = anim_states_();
        if (!states.empty()) anim_open_clip_(states.front().name);
    }
    if (app_.anim_playing_ && app_.anim_clip_.open()) {
        app_.anim_time_ += dt;
        const float len = std::max(app_.anim_clip_.model.length, 1e-3f);
        if (app_.anim_time_ > len) {
            if (app_.anim_clip_.model.wrap == "once") { app_.anim_time_ = len; app_.anim_playing_ = false; }
            else app_.anim_time_ = std::fmod(app_.anim_time_, len);
        }
    }
    if (!anim_preview_wanted_()) {
        // Pose mode holds the live pose even with no clip open -- posing comes first, keying
        // only once there is somewhere to key into.
        if (app_.in_pose_mode_()) { app_.anim_posed_ = true; app_.anim_playing_ = false; return; }
        if (app_.anim_posed_) { anim_restore_rest_pose_(); app_.anim_posed_ = false; }
        app_.anim_playing_ = false;
        return;
    }
    if (app_.anim_runtime_rev_ != app_.anim_clip_.revision || !app_.anim_runtime_) {
        app_.anim_runtime_ = std::make_shared<coopa::anim::AnimationClip>(coopa::anim::parse_clip(app_.anim_clip_.model.to_node()));
        app_.anim_runtime_rev_ = app_.anim_clip_.revision;
        app_.anim_pose_time_ = -1.0f;
    }
    // Pose mode and Record keep hand-posed (unkeyed) changes until the playhead or the keys
    // move; otherwise the clip drives the pose every frame (a document edit cannot unpose it).
    const bool hold = app_.in_pose_mode_() || app_.anim_record_;
    const bool changed = app_.anim_pose_time_ != app_.anim_time_ || app_.anim_pose_rev_ != app_.anim_runtime_rev_ || !app_.anim_posed_;
    if (!hold || changed) {
        if (!app_.anim_posed_) anim_restore_rest_pose_();   // unkeyed channels start from rest
        apply_clip_pose(*app_.anim_runtime_, app_.anim_time_, app_.sync_.live(app_.anim_rig_));
        app_.anim_pose_time_ = app_.anim_time_;
        app_.anim_pose_rev_ = app_.anim_runtime_rev_;
    }
    app_.anim_posed_ = true;
}

void TimelinePanel::anim_insert_keys_() {
    if (!app_.anim_rig_) return;
    if (!app_.anim_clip_.open()) {
        if (anim_states_().empty()) anim_new_clip_();
        else anim_open_clip_(anim_states_().front().name);
    }
    if (!app_.anim_clip_.open()) return;
    std::vector<ObjectId> targets;
    for (ObjectId id : app_.doc_.selection()) if (in_anim_rig_(id)) targets.push_back(id);
    if (targets.empty()) targets.push_back(app_.anim_rig_);
    const float t = app_.anim_time_;
    struct Pose { std::string path; glm::vec3 p; glm::quat q; glm::vec3 s; };
    std::vector<Pose> poses;
    for (ObjectId id : targets) {
        auto* live = app_.sync_.live(id);
        if (!live || !live->get_transform()) continue;
        const auto& tr = live->get_transform()->transform();
        poses.push_back({rig_path_of_(app_.anim_rig_, id), tr.position(), tr.rotation_quat(), tr.scale()});
    }
    if (poses.empty()) return;
    anim_clip_edit_("Insert Keyframe", [&](ClipModel& m) {
        for (const auto& ps : poses) {
            m.set_key(ps.path, "position", t, glm::vec4(ps.p, 0.0f));
            m.set_key(ps.path, "rotation_quat", t, glm::vec4(ps.q.x, ps.q.y, ps.q.z, ps.q.w));
            m.set_key(ps.path, "scale", t, glm::vec4(ps.s, 0.0f));
        }
        if (t > m.length) m.length = t;
    });
    app_.anim_pose_rev_ = 0;   // re-pose from the new keys (still the same pose at this frame)
}

void TimelinePanel::anim_delete_selected_keys_() {
    if (app_.anim_sel_keys_.empty()) return;
    const auto refs = app_.anim_sel_keys_;
    anim_clip_edit_("Delete Keyframes", [&](ClipModel& m) { m.delete_keys(refs); });
    app_.anim_sel_keys_.clear();
}

void TimelinePanel::anim_key_channel_(ObjectId id, const std::string& property) {
    if (!app_.anim_clip_.open() || !in_anim_rig_(id)) return;
    auto* live = app_.sync_.live(id);
    if (!live || !live->get_transform()) return;
    const auto& tr = live->get_transform()->transform();
    glm::vec4 v;
    if (property == "position") v = glm::vec4(tr.position(), 0.0f);
    else if (property == "scale") v = glm::vec4(tr.scale(), 0.0f);
    else { const glm::quat q = tr.rotation_quat(); v = glm::vec4(q.x, q.y, q.z, q.w); }
    const std::string path = rig_path_of_(app_.anim_rig_, id);
    const float t = app_.anim_time_;
    anim_clip_edit_("Insert Keyframe", [&](ClipModel& m) {
        m.set_key(path, property, t, v);
        if (t > m.length) m.length = t;
    });
}

bool TimelinePanel::anim_keyed_now_(ObjectId id, const std::string& property) const {
    if (!app_.anim_clip_.open() || !in_anim_rig_(id)) return false;
    for (float k : app_.anim_clip_.model.key_times(rig_path_of_(app_.anim_rig_, id), property)) {
        if (std::abs(k - app_.anim_time_) <= 0.5f / kAnimFps) return true;
    }
    return false;
}

void TimelinePanel::anim_request_delete_clip_() {
    if (!app_.anim_rig_ || !app_.anim_clip_.open()) return;
    // An inherited state's clip file belongs to the object asset that declares it; dropping the
    // state here is an override and never touches the file, so there is nothing to warn about.
    if (anim_state_is_inherited_(app_.anim_clip_.state)) { anim_delete_clip_(true); return; }
    const std::vector<std::string> others = anim_clip_sharers_();
    if (others.empty()) { anim_delete_clip_(false); return; }
    clip_share_prompt_ = ClipSharePrompt{ClipShareAction::Delete, app_.anim_clip_.state, {}, others};
    app_.pending_modal_ = "Shared Clip";
}

void TimelinePanel::anim_request_rename_clip_(const std::string& to) {
    if (!app_.anim_rig_ || !app_.anim_clip_.open() || to.empty() || to == app_.anim_clip_.state) return;
    const std::vector<std::string> others = anim_clip_sharers_();
    if (others.empty()) { anim_rename_clip_(to, false); return; }
    clip_share_prompt_ = ClipSharePrompt{ClipShareAction::Rename, app_.anim_clip_.state, to, others};
    app_.pending_modal_ = "Shared Clip";
}

void TimelinePanel::anim_rename_clip_(const std::string& to, bool repoint_others) {
    if (!app_.anim_rig_ || !app_.anim_clip_.open() || to.empty() || to == app_.anim_clip_.state) return;
    for (const auto& s : anim_states_()) if (s.name == to) { app_.log_warn("Timeline: a clip named " + to + " exists"); return; }
    // Renaming a state the object asset declares would rename it for this rig alone and leave
    // the asset playing the old name. Send the user to the asset instead of forking the set.
    if (anim_state_is_inherited_(app_.anim_clip_.state)) {
        app_.log_warn("Timeline: " + app_.anim_clip_.state + " comes from " + anim_instance_asset_(app_.anim_rig_) +
                 "; rename it there so every rig that inherits it follows");
        return;
    }
    const fs::path old_path = app_.anim_clip_.path;
    const fs::path new_path = old_path.parent_path() / (to + ".yaml");
    // Both resolved before the file moves: the referrer index is keyed by resolved path.
    const std::vector<std::string> others = repoint_others ? anim_clip_sharers_() : std::vector<std::string>{};
    const std::string old_rel = app_.project_.relative(old_path), new_rel = app_.project_.relative(new_path);
    std::error_code ec;
    fs::rename(old_path, new_path, ec);
    if (ec) { app_.log_error("Timeline: could not rename the clip file: " + ec.message()); return; }
    if (!others.empty() && !old_rel.empty() && !new_rel.empty()) {
        const std::vector<std::string> done = app_.project_.repoint_clip_refs(others, old_rel, new_rel);
        app_.log_info("Timeline: repointed " + std::to_string(done.size()) + " other object(s) at " + new_rel);
        // A safety net: anything that could not be rewritten (an unparsable file, a spelling
        // RefRename does not cover) is still pointing at a file that has moved, so name it.
        std::string left;
        for (const std::string& who : others) {
            if (std::find(done.begin(), done.end(), who) == done.end()) left += (left.empty() ? "" : ", ") + who;
        }
        if (!left.empty()) {
            app_.log_warn("Timeline: " + std::to_string(others.size() - done.size()) +
                     " object(s) name this clip some other way and still point at " + old_rel + ": " + left);
        }
    }
    const std::string from = app_.anim_clip_.state;
    anim_edit_animator_([&](Node& comp) {
        if (!comp.contains("states") || !comp.at("states").is_sequence()) return;
        for (auto& st : comp["states"].as_seq()) {
            if (get_string(st, "name") != from) continue;
            st["name"] = Node(to);
            const std::string rel = get_string(st, "clip");
            st["clip"] = Node(rel.substr(0, rel.find_last_of('/') + 1) + to + ".yaml");
        }
        if (get_string(comp, "auto_play") == from) comp["auto_play"] = Node(to);
    }, "Rename Clip");
    app_.anim_clip_.state = to;
    app_.anim_clip_.path = new_path;
    app_.anim_clip_.model.name = to;
    anim_save_clip_();
    app_.log_info("Timeline: renamed clip to " + to);
}

std::vector<std::string> TimelinePanel::anim_clip_sharers_() {
    if (!app_.anim_clip_.open()) return {};
    std::vector<std::string> others = app_.project_.clip_referrers(app_.anim_clip_.path);
    // The open document plays it too, of course; what matters is who ELSE an edit reaches.
    const std::string self = app_.doc_.path().empty() ? std::string() : app_.project_.relative(app_.doc_.path());
    others.erase(std::remove(others.begin(), others.end(), self), others.end());
    return others;
}

void TimelinePanel::draw_clip_sharing_(imm::Context& ctx, const imm::Box& hb, float& x) {
    using I = imm::Icon;
    const std::vector<std::string> others = anim_clip_sharers_();
    if (others.empty()) return;

    const float bh = hb.h - 6;
    const std::string label = "Shared by " + std::to_string(others.size());
    const float w = ctx.text_width(label) + bh + 16;
    const imm::Box b{x, hb.y + 3, w, bh};
    bool hov = false, held = false;
    if (ctx.invisible_button("clip_shared", b, &hov, &held)) {
        ctx.open_popup("clip_sharers", glm::vec2(b.x, b.bottom() + 2));
    }
    ctx.fill_rounded(b, imm::with_alpha(ctx.style.warning, hov ? 0.34f : 0.20f));
    ctx.icon(I::Link, {b.x + 3, b.y + 2, bh - 4, bh - 4}, ctx.style.warning);
    ctx.text_in({b.x + bh + 2, b.y, w - bh - 6, bh}, label, ctx.style.text, 0.0f);
    ctx.tooltip("Shared Clip\n" + app_.project_.relative(app_.anim_clip_.path) + " is played by " +
                std::to_string(others.size()) + " other object" + (others.size() == 1 ? "" : "s") +
                ".\nEvery key you move here moves for all of them. Click to list them");
    app_.test_rects_["clip_shared"] = b;

    if (ctx.begin_popup("clip_sharers", 360)) {
        ctx.label_dim(app_.project_.relative(app_.anim_clip_.path));
        ctx.menu_separator();
        for (const std::string& who : others) {
            const bool is_object = fs::path(who).generic_string().rfind("objects/", 0) == 0;
            if (ctx.menu_item(strip_yaml_ext(who), "", nullptr, is_object, I::Object) && is_object) {
                app_.assets_panel_->open_object_asset(app_.project_.assets() / who);
            }
        }
        ctx.end_popup();
    }
    x += w + 6;
}

void TimelinePanel::draw_clip_share_modal_(imm::Context& ctx) {
    if (ctx.begin_modal("Shared Clip", {430, 0})) {   // height fits the content
        const ClipSharePrompt p = clip_share_prompt_;
        const std::string n = std::to_string(p.others.size());
        const std::string objs = n + " other object" + (p.others.size() == 1 ? "" : "s");
        if (p.action == ClipShareAction::Rename) {
            ctx.paragraph("This clip's file is also played by " + objs +
                          ". Renaming the file leaves them pointing at a name that no longer exists.");
            ctx.spacing(4);
            if (ctx.button("Rename for All", 150)) {
                ctx.close_modal();
                anim_rename_clip_(p.rename_to, true);
            }
            ctx.same_line();
            if (ctx.button("Cancel", 110)) { ctx.close_modal(); clip_share_prompt_ = {}; }
        } else {
            ctx.paragraph("This clip's file is also played by " + objs +
                          ". Removing the state leaves their clips untouched; deleting the file breaks them.");
            ctx.spacing(4);
            if (ctx.button("Remove State", 130)) {
                ctx.close_modal();
                anim_delete_clip_(true);
                clip_share_prompt_ = {};
            }
            ctx.same_line();
            if (ctx.button("Delete File", 120)) {
                ctx.close_modal();
                anim_delete_clip_(false);
                clip_share_prompt_ = {};
            }
            ctx.same_line();
            if (ctx.button("Cancel", 100)) { ctx.close_modal(); clip_share_prompt_ = {}; }
        }
        ctx.end_modal();
    }
}

void TimelinePanel::draw_timeline_(imm::Context& ctx, const imm::Box& hb, const imm::Box& body, float x) {
    using I = imm::Icon;
    const float bh = hb.h - 6;
    app_.timeline_hovered_ = ctx.is_hovered(body) || ctx.is_hovered(hb);
    if (!app_.anim_rig_) {
        const ObjectId sel = app_.doc_.primary();
        // A placed object asset: its rig and clips live in the asset -- animate it there.
        if (const std::string inst = sel ? anim_instance_asset_(sel) : std::string(); !inst.empty()) {
            ctx.text_in({x, hb.y, 330, hb.h}, "Placed from " + inst + ": animate it in its object asset", ctx.style.text_dim, 0.0f);
            ctx.push_id("openinst");
            const imm::Box b{x + 330, hb.y + 3, 140, bh};
            bool hov = false, held = false;
            if (ctx.invisible_button("open", b, &hov, &held)) {
                const fs::path p = app_.project_.assets() / (inst + (fs::path(inst).has_extension() ? "" : ".yaml"));
                app_.assets_panel_->open_object_asset(p);
            }
            ctx.fill_rounded(b, hov ? ctx.style.button_hover : ctx.style.button);
            ctx.text_in(b, "Open Object Asset", ctx.style.text, 0.0f, true);
            ctx.tooltip("Open " + inst + "\nIts Timeline edits the clips every placed copy plays");
            ctx.pop_id();
            return;
        }
        ctx.text_in({x, hb.y, 360, hb.h}, sel ? "No Animator on this object or its parents" : "Select an object to animate",
                    ctx.style.text_dim, 0.0f);
        if (sel && (app_.active_type_ == AssetType::Scene || app_.active_type_ == AssetType::Object)) {
            ctx.push_id("addanim");
            const imm::Box b{x + 290, hb.y + 3, 120, bh};
            bool hov = false, held = false;
            if (ctx.invisible_button("add", b, &hov, &held)) anim_add_animator_(sel);
            ctx.fill_rounded(b, hov ? ctx.style.button_hover : ctx.style.button);
            ctx.text_in(b, "Add Animator", ctx.style.text, 0.0f, true);
            ctx.tooltip("Add Animator\nMake this object a rig: its clips animate it and everything under it");
            ctx.pop_id();
        }
        return;
    }
    const std::string rig_name = anim_name_of_(app_.anim_rig_, "Rig");
    // Clip dropdown.
    const std::string clip_label = app_.anim_clip_.open() ? app_.anim_clip_.state : std::string("No Clip");
    const float cw = std::max(110.0f, ctx.text_width(clip_label) + bh + 20);
    {
        const imm::Box b{x, hb.y + 3, cw, bh};
        bool hov = false, held = false;
        if (ctx.invisible_button("clip_dd", b, &hov, &held)) ctx.open_popup("clip_menu", glm::vec2(b.x, b.bottom() + 2));
        ctx.fill_rounded(b, hov ? ctx.style.button_hover : ctx.style.button);
        ctx.icon(I::Play, {b.x + 3, b.y + 2, bh - 4, bh - 4}, ctx.style.text_dim);
        ctx.text_in({b.x + bh + 2, b.y, cw - bh - 16, bh}, clip_label, ctx.style.text, 0.0f);
        ctx.arrow({b.right() - 14, b.y + 4, 10, bh - 8}, true, ctx.style.text_dim);
        ctx.tooltip("Clip\n" + rig_name + "'s clips (its Animator's states)." +
                    (app_.anim_clip_.open() ? "\nOpen: " + app_.project_.relative(app_.anim_clip_.path) : std::string()));
        if (ctx.begin_popup("clip_menu", 200)) {
            for (const AnimStateRef& st : anim_states_()) {
                bool on = st.name == app_.anim_clip_.state;
                if (ctx.menu_item(st.name, "", &on)) { anim_open_clip_(st.name); app_.anim_time_ = 0.0f; }
            }
            ctx.menu_separator();
            if (ctx.menu_item("New Clip", "", nullptr, true, I::Plus)) anim_new_clip_();
            if (app_.anim_clip_.open()) {
                if (app_.anim_rename_buf_.empty()) app_.anim_rename_buf_ = app_.anim_clip_.state;
                if (ctx.input_text("Rename", &app_.anim_rename_buf_)) anim_request_rename_clip_(app_.anim_rename_buf_);
            }
            if (ctx.menu_item("Delete Clip", "", nullptr, app_.anim_clip_.open(), I::Trash)) anim_request_delete_clip_();
            ctx.end_popup();
        }
        x += cw + 6;
    }
    draw_clip_sharing_(ctx, hb, x);
    if (!app_.anim_clip_.open()) {
        ctx.text_in({x, hb.y, 260, hb.h}, "Rig " + rig_name + ": add a clip to start animating", ctx.style.text_dim, 0.0f);
        return;
    }
    auto icon_btn = [&](const char* id, I ic, const char* tip, bool on) {
        const bool r = ctx.icon_button(id, ic, tip, on, bh, imm::Context::kAll, imm::Box{x, hb.y + 3, bh, bh});
        x += bh + 3;
        return r;
    };
    if (icon_btn("tl_start", I::Undo, "Jump to Start\nHome", false)) app_.anim_time_ = 0.0f;
    if (icon_btn("tl_play", app_.anim_playing_ ? I::Pause : I::Play, "Play / Pause\nSpace (while the Timeline is open)", app_.anim_playing_)) {
        app_.anim_playing_ = !app_.anim_playing_;
    }
    x += 4;
    float frame = std::round(app_.anim_time_ * kAnimFps);
    ctx.drag_float_box("tl_frame", {x, hb.y + 3, 56, bh}, &frame, 0.2f, 0.0f, 100000.0f, "%.0f");
    ctx.tooltip("Frame\nThe playhead (30 frames a second)");
    app_.anim_time_ = frame / kAnimFps;
    x += 62;
    // Record: rig transforms change the live pose only.
    {
        const imm::Box b{x, hb.y + 3, bh, bh};
        bool hov = false, held = false;
        if (ctx.invisible_button("tl_rec", b, &hov, &held)) app_.anim_record_ = !app_.anim_record_;
        ctx.fill_rounded(b, app_.anim_record_ ? glm::vec4(0.55f, 0.12f, 0.12f, 1) : hov ? ctx.style.button_hover : ctx.style.button);
        ctx.circle(b.center(), bh * 0.28f, app_.anim_record_ ? glm::vec4(1, 0.3f, 0.3f, 1) : glm::vec4(0.85f, 0.25f, 0.25f, 1));
        ctx.tooltip("Record\nPose the rig for keys: moving its objects changes only the animated pose, never the rest pose. "
                    "I keys the selection.");
        x += bh + 3;
    }
    if (icon_btn("tl_key", I::Plus, "Insert Keyframe\nI: key position, rotation and scale of the selected rig objects", false)) {
        anim_insert_keys_();
    }
    if (icon_btn("tl_rest", I::ObjectMode, "Rest Pose\nShow the rig as authored instead of the clip", app_.anim_show_rest_)) {
        app_.anim_show_rest_ = !app_.anim_show_rest_;
    }
    if (icon_btn("tl_all", I::Collection, "All Objects\nList every object of the rig (off: the animated and selected ones)",
                 app_.anim_show_all_)) {
        app_.anim_show_all_ = !app_.anim_show_all_;
    }
    x += 6;
    ctx.text_in({x, hb.y, 44, hb.h}, "Length", ctx.style.text_dim, 0.0f);
    x += 44;
    float len_frames = std::round(app_.anim_clip_.model.length * kAnimFps);
    if (ctx.drag_float_box("tl_len", {x, hb.y + 3, 52, bh}, &len_frames, 0.2f, 1.0f, 100000.0f, "%.0f")) {
        const float l = len_frames / kAnimFps;
        anim_clip_edit_("Clip Length", [l](ClipModel& m) { m.length = l; });
    }
    ctx.tooltip("Length\nThe clip's length in frames");
    x += 58;
    {
        static const char* kWraps[] = {"loop", "once", "pingpong"};
        int w = 0;
        for (int i = 0; i < 3; ++i) if (app_.anim_clip_.model.wrap == kWraps[i]) w = i;
        const imm::Box b{x, hb.y + 3, 78, bh};
        bool hov = false, held = false;
        if (ctx.invisible_button("tl_wrap", b, &hov, &held)) {
            const std::string nw = kWraps[(w + 1) % 3];
            anim_clip_edit_("Clip Wrap", [nw](ClipModel& m) { m.wrap = nw; });
        }
        ctx.fill_rounded(b, hov ? ctx.style.button_hover : ctx.style.button);
        ctx.text_in(b, kWraps[w], ctx.style.text, 0.0f, true);
        ctx.tooltip("Wrap\nLoop / play once and hold / ping-pong (click cycles)");
        x += 84;
    }
    draw_dope_sheet_(ctx, body);
}

std::string TimelinePanel::anim_channel_label_(const std::string& property) {
    if (property == "position") return "Position";
    if (property == "rotation_quat" || property == "rotation") return "Rotation";
    if (property == "scale") return "Scale";
    return property;
}

void TimelinePanel::draw_dope_sheet_(imm::Context& ctx, const imm::Box& body) {
    const auto& in = ctx.input();
    const float name_w = 190.0f, ruler_h = 20.0f, row_h = 20.0f;
    const imm::Box names{body.x, body.y, name_w, body.h};
    const imm::Box track{body.x + name_w, body.y, body.w - name_w, body.h};
    ctx.fill(body, ctx.style.panel_bg);
    ctx.fill(names, ctx.style.panel_alt);
    const float len = std::max(app_.anim_clip_.model.length, 1.0f / kAnimFps);
    // Visible range (seconds): fit the clip (plus a little) until zoomed or panned.
    if (app_.anim_view_.end <= app_.anim_view_.start) app_.anim_view_ = {0.0f, len * 1.04f + 2.0f / kAnimFps};
    const float x0 = track.x + 12, x1 = track.right() - 12;
    const float pps = (x1 - x0) / std::max(app_.anim_view_.end - app_.anim_view_.start, 1e-3f);
    auto x_of = [&](float t) { return x0 + (t - app_.anim_view_.start) * pps; };
    auto t_of = [&](float x) { return app_.anim_view_.start + (x - x0) / pps; };
    const glm::vec2 m = ctx.mouse();

    // Rows scroll with the wheel over the names column.
    if (ctx.is_hovered(names) && in.scroll.y != 0.0f && !ctx.any_popup_open()) app_.anim_row_scroll_ -= static_cast<int>(in.scroll.y);
    // Zoom (wheel, about the mouse) and pan (Shift-wheel, middle-drag).
    if (ctx.is_hovered(track) && !ctx.any_popup_open()) {
        if (in.scroll.y != 0.0f && !has(in.mods, Mods::Shift)) {
            const float at = t_of(m.x);
            const float k = std::pow(0.85f, in.scroll.y);
            app_.anim_view_.start = at - (at - app_.anim_view_.start) * k;
            app_.anim_view_.end = at + (app_.anim_view_.end - at) * k;
        } else if (in.scroll.y != 0.0f || in.scroll.x != 0.0f) {
            const float d = -(in.scroll.y + in.scroll.x) * 30.0f / pps;
            app_.anim_view_.start += d;
            app_.anim_view_.end += d;
        }
        if (in.down[2] && in.mouse_delta.x != 0.0f) {
            app_.anim_view_.start -= in.mouse_delta.x / pps;
            app_.anim_view_.end -= in.mouse_delta.x / pps;
        }
    }

    // Ruler.
    const imm::Box ruler{track.x, track.y, track.w, ruler_h};
    ctx.fill(ruler, ctx.style.header);
    ctx.push_clip(track);
    int label_every = 1;
    while (label_every * pps / kAnimFps < 34.0f) label_every *= (label_every == 1 ? 5 : 2);
    const int f0 = static_cast<int>(std::floor(app_.anim_view_.start * kAnimFps)), f1 = static_cast<int>(std::ceil(app_.anim_view_.end * kAnimFps));
    for (int f = std::max(f0, -1000000); f <= f1; ++f) {
        const float x = x_of(f / kAnimFps);
        const bool major = f % label_every == 0;
        if (!major && pps / kAnimFps < 5.0f) continue;
        ctx.line({x, ruler.bottom() - (major ? 7.0f : 3.0f)}, {x, ruler.bottom()}, ctx.style.text_dim, 1.0f);
        if (major) {
            // Centred on its tick, like the playhead's tag, so a key on this frame sits under its number.
            const std::string label = std::to_string(f);
            ctx.draw_text({x - ctx.text.width(label, 10.0f) * 0.5f, ruler.y + 2}, label, ctx.style.text_dim, 10.0f);
        }
        if (major) ctx.line({x, ruler.bottom()}, {x, track.bottom()}, imm::with_alpha(ctx.style.text_dim, 0.12f), 1.0f);
    }
    // Outside the clip's range is shaded; its end is a line.
    if (x_of(0.0f) > track.x) ctx.fill({track.x, ruler.bottom(), x_of(0.0f) - track.x, track.h - ruler_h}, glm::vec4(0, 0, 0, 0.18f));
    if (x_of(len) < track.right()) ctx.fill({x_of(len), ruler.bottom(), track.right() - x_of(len), track.h - ruler_h}, glm::vec4(0, 0, 0, 0.18f));
    ctx.line({x_of(len), track.y}, {x_of(len), track.bottom()}, imm::with_alpha(ctx.style.accent, 0.6f), 1.0f);
    ctx.pop_clip();

    // Rows: Summary, each rig object, and an expanded object's channels.
    struct Row { std::string label; std::string path; std::string property; bool summary; ObjectId id; int depth; bool events = false; };
    std::vector<Row> rows{{"Summary", "", "", true, 0, 0}, {"Events", "", "", false, 0, 0, true}};
    // Blender's dope sheet: the rig root, what is animated and what is selected -- unless
    // "All objects" (the header's list button) shows every object of the rig.
    std::set<std::string> animated;
    for (const auto& t : app_.anim_clip_.model.tracks) animated.insert(t.object);
    for (const auto& [id, depth] : rig_objects_(app_.anim_rig_)) {
        const std::string path = rig_path_of_(app_.anim_rig_, id);
        if (!app_.anim_show_all_ && id != app_.anim_rig_ && !animated.count(path) && !app_.doc_.is_selected(id)) continue;
        rows.push_back({anim_name_of_(id), path, "", false, id, app_.anim_show_all_ ? depth : (id == app_.anim_rig_ ? 0 : 1)});
        if (!app_.anim_expanded_.count(path)) continue;
        std::vector<std::string> props;
        for (const auto& t : app_.anim_clip_.model.tracks) {
            if (t.object == path && !t.is_procedural() && std::find(props.begin(), props.end(), t.property) == props.end()) props.push_back(t.property);
        }
        for (const auto& prop : props) rows.push_back({anim_channel_label_(prop), path, prop, false, id, depth + 1});
    }
    auto row_times = [&](const Row& r) {
        if (r.events) return std::vector<float>{};
        if (r.summary) return app_.anim_clip_.model.key_times();
        if (!r.property.empty()) return app_.anim_clip_.model.key_times(r.path, r.property);
        return app_.anim_clip_.model.key_times(&r.path);
    };
    // The dope-sheet cells a diamond stands for.
    auto cells_of = [&](const Row& r, float t) {
        std::set<ClipModel::KeyRef> cells;
        if (r.summary) {
            for (const auto& rw : rows) {
                if (rw.summary || rw.events || !rw.property.empty()) continue;
                for (float kt : app_.anim_clip_.model.key_times(&rw.path)) if (std::abs(kt - t) <= ClipModel::kTimeEps) cells.insert({rw.path, kt, ""});
            }
        } else {
            cells.insert({r.path, t, r.property});
        }
        return cells;
    };
    auto is_selected = [&](const Row& r, float t) {
        for (const auto& k : app_.anim_sel_keys_) {
            if (std::abs(k.time - t) > ClipModel::kTimeEps) continue;
            if (r.summary) return true;
            if (k.object == r.path && (k.property.empty() || k.property == r.property)) return true;
        }
        return false;
    };
    const bool in_track = ctx.is_hovered(track) && m.y > ruler.bottom();
    // Scrolling: Summary and Events stay pinned; the rest start at anim_row_scroll_.
    constexpr int kPinned = 2;
    const int visible = std::max(1, static_cast<int>((body.h - ruler_h) / row_h) - kPinned);
    app_.anim_row_scroll_ = std::clamp(app_.anim_row_scroll_, 0, std::max(0, static_cast<int>(rows.size()) - kPinned - visible));
    if (app_.anim_row_scroll_ > 0) rows.erase(rows.begin() + kPinned, rows.begin() + kPinned + app_.anim_row_scroll_);
    if (static_cast<int>(rows.size()) - kPinned > visible || app_.anim_row_scroll_ > 0) {
        // A thin scrollbar on the names column's right edge.
        const float total = static_cast<float>(rows.size() - kPinned + app_.anim_row_scroll_);
        const float h = body.h - ruler_h;
        ctx.fill_rounded({names.right() - 4, body.y + ruler_h + h * app_.anim_row_scroll_ / total, 3, h * visible / total},
                         imm::with_alpha(ctx.style.text_dim, 0.5f), 1.5f);
    }
    bool on_key = false;
    ctx.push_clip(body);
    for (size_t r = 0; r < rows.size(); ++r) {
        const float y = body.y + ruler_h + r * row_h;
        if (y > body.bottom()) break;
        const Row& row = rows[r];
        const imm::Box rb{body.x, y, body.w, row_h};
        if (r % 2) ctx.fill(rb, glm::vec4(1, 1, 1, 0.025f));
        const bool selected = !row.summary && row.property.empty() && app_.doc_.is_selected(row.id);
        if (selected) ctx.fill({names.x, y, names.w, row_h}, imm::with_alpha(ctx.style.selection, 0.6f));
        float tx = names.x + 6 + row.depth * 12.0f;
        // The expand arrow (objects with tracks).
        if (!row.summary && row.property.empty()) {
            bool has_tracks = false;
            for (const auto& t : app_.anim_clip_.model.tracks) has_tracks |= t.object == row.path && !t.is_procedural();
            if (has_tracks) {
                const imm::Box ab{tx, y + 4, 12, row_h - 8};
                const bool open = app_.anim_expanded_.count(row.path) > 0;
                ctx.arrow(ab, open, ctx.style.text_dim);
                if (ctx.is_hovered(ab) && in.pressed[0]) {
                    if (open) app_.anim_expanded_.erase(row.path); else app_.anim_expanded_.insert(row.path);
                }
            }
            tx += 14;
        }
        const glm::vec4 label_col = row.summary || row.events ? ctx.style.text_dim : !row.property.empty() ? ctx.style.text_dim : ctx.style.text;
        ctx.text_in({tx, y, names.right() - tx - 4, row_h}, row.label, label_col, 0.0f);
        if (row.events) {
            draw_events_lane_(ctx, track, y, row_h, x_of, t_of, pps, in_track, on_key);
            continue;
        }
        // Clicking a name selects the object (Shift adds).
        if (!row.summary && row.property.empty() && ctx.is_hovered({tx, y, names.right() - tx, row_h}) && in.pressed[0]) {
            app_.doc_.select(row.id, has(in.mods, Mods::Shift));
        }
        ctx.push_clip(track);
        for (float t : row_times(row)) {
            const bool sel_key = is_selected(row, t);
            const glm::vec2 c{x_of(t) + (app_.anim_drag_.active && sel_key ? app_.anim_drag_.dt * pps : 0.0f), y + row_h * 0.5f};
            const float rr = row.summary ? 5.5f : !row.property.empty() ? 4.0f : 4.5f;
            const glm::vec4 fillc = sel_key ? glm::vec4(1.0f, 0.62f, 0.18f, 1) : row.summary ? glm::vec4(0.95f, 0.95f, 0.95f, 1)
                                                                                : glm::vec4(0.78f, 0.78f, 0.78f, 1);
            ctx.triangle({c.x, c.y - rr}, {c.x + rr, c.y}, {c.x, c.y + rr}, fillc);
            ctx.triangle({c.x, c.y - rr}, {c.x, c.y + rr}, {c.x - rr, c.y}, fillc);
            if (!in_track || on_key || glm::distance(m, c) > rr + 2.0f) continue;
            if (in.pressed[0]) {
                on_key = true;
                app_.anim_sel_event_ = -1;
                const auto cells = cells_of(row, t);
                if (has(in.mods, Mods::Shift)) {
                    for (const auto& c2 : cells) {
                        if (app_.anim_sel_keys_.count(c2)) app_.anim_sel_keys_.erase(c2);
                        else app_.anim_sel_keys_.insert(c2);
                    }
                } else if (!sel_key) {
                    app_.anim_sel_keys_ = cells;
                }
                app_.anim_drag_ = {true, m.x, 0.0f};
            } else if (in.pressed[1]) {
                on_key = true;
                if (!sel_key) app_.anim_sel_keys_ = cells_of(row, t);
                app_.anim_menu_time_ = t;
                ctx.open_popup("key_menu", m);
            }
        }
        ctx.pop_clip();
    }
    ctx.pop_clip();

    // Dragging keys: snapped to frames, applied as one edit on release.
    if (app_.anim_drag_.active) {
        if (in.down[0]) {
            app_.anim_drag_.dt = std::round((m.x - app_.anim_drag_.start_x) / pps * kAnimFps) / kAnimFps;
        } else {
            const float d = app_.anim_drag_.dt;
            app_.anim_drag_.active = false;
            if (std::abs(d) > 1e-6f) {
                const auto refs = app_.anim_sel_keys_;
                std::set<ClipModel::KeyRef> moved;
                anim_clip_edit_("Move Keyframes", [&](ClipModel& mm) { moved = mm.move_keys(refs, d); });
                app_.anim_sel_keys_ = moved;
            }
        }
    } else if (in_track && !on_key) {
        if (in.pressed[0]) { app_.anim_sel_keys_.clear(); app_.anim_sel_event_ = -1; }   // empty space
        if (in.pressed[1]) {
            app_.anim_menu_time_ = anim_snap_(t_of(m.x));
            ctx.open_popup("track_menu", m);
        }
    }

    // Dragging an event flag: snapped to frames, one edit on release.
    if (app_.anim_event_drag_.active) {
        if (in.down[0]) {
            app_.anim_event_drag_.dt = std::round((m.x - app_.anim_event_drag_.start_x) / pps * kAnimFps) / kAnimFps;
        } else {
            const float d = app_.anim_event_drag_.dt;
            app_.anim_event_drag_.active = false;
            if (std::abs(d) > 1e-6f && app_.anim_sel_event_ >= 0 && app_.anim_sel_event_ < static_cast<int>(app_.anim_clip_.model.events.size())) {
                const size_t idx = static_cast<size_t>(app_.anim_sel_event_);
                const float to = anim_snap_(app_.anim_clip_.model.events[idx].time + d);
                int moved = app_.anim_sel_event_;
                anim_clip_edit_("Move Event", [&](ClipModel& mm) { moved = mm.move_event(idx, to); });
                app_.anim_sel_event_ = moved;
            }
        }
    }

    // Right-click menus.
    if (ctx.begin_popup("events_lane_menu", 190)) {
        if (ctx.menu_item("Add Event Here", "", nullptr, true, imm::Icon::Plus)) anim_add_event_(app_.anim_menu_time_, "event");
        ctx.end_popup();
    }
    if (ctx.begin_popup("event_menu", 230)) {
        if (app_.anim_sel_event_ >= 0 && app_.anim_sel_event_ < static_cast<int>(app_.anim_clip_.model.events.size())) {
            const size_t idx = static_cast<size_t>(app_.anim_sel_event_);
            const ClipEvent ev = app_.anim_clip_.model.events[idx];
            ctx.label_dim("Event at frame " + std::to_string(static_cast<int>(std::round(ev.time * kAnimFps))));
            std::string nm = ev.name;
            if (ctx.input_text("Name", &nm) && !nm.empty()) {
                anim_clip_edit_("Rename Event", [&](ClipModel& mm) { mm.events[idx].name = nm; });
            }
            ctx.tooltip("Name\nWhat listeners match on (Animator::on_event, the \"anim_event\" signal's name)");
            std::string sv = ev.string_value;
            if (ctx.input_text("String", &sv)) {
                anim_clip_edit_("Event String", [&](ClipModel& mm) { mm.events[idx].string_value = sv; });
            }
            ctx.tooltip("String\nAn optional text payload (e.g. left / right for a footstep)");
            float fv = ev.float_value;
            if (ctx.drag_float("Float", &fv, 0.01f)) {
                anim_clip_edit_("Event Float", [&](ClipModel& mm) { mm.events[idx].float_value = fv; }, "event_float");
            }
            ctx.menu_separator();
            if (ctx.menu_item("Jump to Event", "")) app_.anim_time_ = ev.time;
            if (ctx.menu_item("Delete Event", "X", nullptr, true, imm::Icon::Trash)) anim_delete_selected_event_();
        }
        ctx.end_popup();
    }
    if (ctx.begin_popup("key_menu", 190)) {
        const std::string cur = app_.anim_clip_.model.easing_of(app_.anim_sel_keys_);
        ctx.label_dim("Interpolation");
        static const char* kEase[][2] = {{"linear", "Linear"}, {"step", "Constant (Step)"}, {"ease_in", "Ease In"},
                                         {"ease_out", "Ease Out"}, {"ease_in_out", "Ease In-Out"}};
        for (const auto& e : kEase) {
            bool on = cur == e[0];
            if (ctx.menu_item(e[1], "", &on)) {
                const auto refs = app_.anim_sel_keys_;
                const std::string ease = e[0];
                anim_clip_edit_("Interpolation", [&](ClipModel& mm) { mm.set_easing(refs, ease); });
            }
        }
        ctx.menu_separator();
        if (ctx.menu_item("Jump to Key", "")) app_.anim_time_ = app_.anim_menu_time_;
        if (ctx.menu_item("Delete Keyframes", "X", nullptr, true, imm::Icon::Trash)) anim_delete_selected_keys_();
        ctx.end_popup();
    }
    if (ctx.begin_popup("track_menu", 190)) {
        if (ctx.menu_item("Insert Keyframe Here", "I")) { app_.anim_time_ = app_.anim_menu_time_; anim_insert_keys_(); }
        if (ctx.menu_item("Select All Keys", "A")) anim_select_all_keys_();
        if (ctx.menu_item("Frame All", "Home")) app_.anim_view_ = {};
        ctx.end_popup();
    }

    // Scrubbing on the ruler.
    if ((ctx.is_hovered(ruler) && in.pressed[0]) || app_.anim_scrubbing_) {
        app_.anim_scrubbing_ = in.down[0];
        const float t = std::max(0.0f, t_of(m.x));
        app_.anim_time_ = has(in.mods, Mods::Shift) ? t : anim_snap_(t);
    }
    // The playhead.
    ctx.push_clip(track);
    const float px = x_of(app_.anim_time_);
    ctx.line({px, track.y}, {px, track.bottom()}, glm::vec4(0.32f, 0.55f, 0.95f, 1), 2.0f);
    const imm::Box tag{px - 15, ruler.y + 1, 30, ruler_h - 2};
    ctx.fill_rounded(tag, glm::vec4(0.32f, 0.55f, 0.95f, 1), 3);
    ctx.text_in(tag, std::to_string(static_cast<int>(std::round(app_.anim_time_ * kAnimFps))), glm::vec4(1), 0.0f, true);
    ctx.pop_clip();

    // Space plays/pauses whenever the Timeline is open -- except in play mode (the game owns the keys)
    // and over the UI designer canvas (Space-drag pans it).
    const bool space_pans = app_.active_type_ == AssetType::UI && app_.viewport_hovered_;
    if (!ctx.wants_keyboard() && !ctx.any_popup_open() && !app_.playing() && !space_pans && ctx.shortcut(Key::Space))
        app_.anim_playing_ = !app_.anim_playing_;
    // Keys over the panel.
    if (app_.timeline_hovered_ && !ctx.wants_keyboard() && !ctx.any_popup_open()) {
        if (ctx.shortcut(Key::I)) anim_insert_keys_();
        if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete)) {
            if (app_.anim_sel_event_ >= 0) anim_delete_selected_event_();
            else anim_delete_selected_keys_();
        }
        if (ctx.shortcut(Key::A)) anim_select_all_keys_();
        if (ctx.shortcut(Key::Right)) app_.anim_time_ = anim_snap_(app_.anim_time_ + 1.0f / kAnimFps);
        if (ctx.shortcut(Key::Left)) app_.anim_time_ = anim_snap_(app_.anim_time_ - 1.0f / kAnimFps);
        if (ctx.shortcut(Key::Up)) app_.anim_time_ = app_.anim_clip_.model.neighbour_key(app_.anim_time_, +1);
        if (ctx.shortcut(Key::Down)) app_.anim_time_ = app_.anim_clip_.model.neighbour_key(app_.anim_time_, -1);
        if (ctx.shortcut(Key::Home)) { app_.anim_time_ = 0.0f; app_.anim_view_ = {}; }
    }
}

void TimelinePanel::anim_add_event_(float time, const std::string& name) {
    if (!app_.anim_clip_.open()) return;
    size_t at = 0;
    anim_clip_edit_("Add Event", [&](ClipModel& m) {
        at = m.add_event(time, name);
        if (time > m.length) m.length = time;
    });
    app_.anim_sel_event_ = static_cast<int>(at);
    app_.anim_sel_keys_.clear();
}

void TimelinePanel::anim_delete_selected_event_() {
    if (app_.anim_sel_event_ < 0) return;
    const size_t idx = static_cast<size_t>(app_.anim_sel_event_);
    anim_clip_edit_("Delete Event", [&](ClipModel& m) { m.delete_event(idx); });
    app_.anim_sel_event_ = -1;
}

void TimelinePanel::anim_select_all_keys_() {
    app_.anim_sel_keys_.clear();
    app_.anim_sel_event_ = -1;
    for (const auto& t : app_.anim_clip_.model.tracks) for (const auto& k : t.keys) app_.anim_sel_keys_.insert({t.object, k.time, ""});
}

void TimelinePanel::show_timeline() { app_.show_bottom_ = true; app_.bottom_view_ = 1; }

void TimelinePanel::set_selected_key_interpolation(const std::string& ease) {
    const auto refs = app_.anim_sel_keys_;
    anim_clip_edit_("Interpolation", [&](ClipModel& m) { m.set_easing(refs, ease); });
}

void TimelinePanel::move_selected_animation_event(float to_time) {
    if (app_.anim_sel_event_ < 0 || app_.anim_sel_event_ >= static_cast<int>(app_.anim_clip_.model.events.size())) return;
    const size_t idx = static_cast<size_t>(app_.anim_sel_event_);
    int moved = app_.anim_sel_event_;
    anim_clip_edit_("Move Event", [&](ClipModel& m) { moved = m.move_event(idx, anim_snap_(to_time)); });
    app_.anim_sel_event_ = moved;
}

void TimelinePanel::rename_selected_animation_event(const std::string& name) {
    if (app_.anim_sel_event_ < 0 || app_.anim_sel_event_ >= static_cast<int>(app_.anim_clip_.model.events.size()) || name.empty()) return;
    const size_t idx = static_cast<size_t>(app_.anim_sel_event_);
    anim_clip_edit_("Rename Event", [&](ClipModel& m) { m.events[idx].name = name; });
}

} // namespace editor
} // namespace toy
