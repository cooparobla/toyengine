#include "editor/app/editor_app.h"

namespace toy {
namespace editor {

ObjectId EditorApp::rig_of_(ObjectId id) const {
    for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = doc_.parent_of(*cur)) {
        if (doc_.find_component(*cur, "Animator") >= 0) return *cur;
    }
    return 0;
}

std::string EditorApp::rig_path_of_(ObjectId rig, ObjectId id) const {
    std::vector<std::string> names;
    for (std::optional<ObjectId> cur = id; cur && *cur != 0 && *cur != rig; cur = doc_.parent_of(*cur)) {
        const Node* n = doc_.find(*cur);
        names.push_back(n ? get_string(*n, "name") : std::string());
    }
    std::string out;
    for (auto it = names.rbegin(); it != names.rend(); ++it) out += (out.empty() ? "" : "/") + *it;
    return out;
}

std::vector<std::pair<ObjectId, int>> EditorApp::rig_objects_(ObjectId rig) const {
    std::vector<std::pair<ObjectId, int>> out;
    if (!rig) return out;
    doc_.for_each_object([&](const Node& n, int depth) {
        const ObjectId id = SceneDocument::id_of(n);
        if (id == rig || doc_.is_ancestor(rig, id)) out.push_back({id, depth});
    });
    if (!out.empty()) {
        const int base = out.front().second;
        for (auto& [id, d] : out) d -= base;
    }
    return out;
}

fs::path EditorApp::anim_resolve_clip_(const std::string& rel) const {
    const fs::path local = anim_scene_dir_() / rel;
    if (coopa::yaml::document_exists(local)) return local;
    const fs::path shared = project_.assets() / rel;
    if (coopa::yaml::document_exists(shared)) return shared;
    return anim_new_clip_base_() / rel;
}

std::string EditorApp::anim_name_of_(ObjectId id, const std::string& fallback) const {
    const Node* n = doc_.find(id);
    return n ? get_string(*n, "name", fallback) : fallback;
}

std::string EditorApp::anim_instance_asset_(ObjectId id) const {
    for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = doc_.parent_of(*cur)) {
        const Node* n = doc_.find(*cur);
        if (!n) break;
        const std::string pf = get_string(*n, "prefab", get_string(*n, "inherit_from"));
        if (!pf.empty()) return pf;
    }
    return {};
}

std::vector<std::pair<std::string, std::string>> EditorApp::anim_states_() const {
    std::vector<std::pair<std::string, std::string>> out;
    if (!anim_rig_) return out;
    const int ci = doc_.find_component(anim_rig_, "Animator");
    if (ci < 0) return out;
    const Node& comp = doc_.find(anim_rig_)->at("components").as_seq()[static_cast<size_t>(ci)];
    if (!comp.contains("states") || !comp.at("states").is_sequence()) return out;
    for (const auto& s : comp.at("states").as_seq()) out.push_back({get_string(s, "name"), get_string(s, "clip")});
    return out;
}

void EditorApp::anim_open_clip_(const std::string& name) {
    for (const auto& [state, clip] : anim_states_()) {
        if (state != name) continue;
        const fs::path p = anim_resolve_clip_(clip);
        anim_clip_ = ClipDocument{};
        anim_clip_.state = state;
        anim_clip_.path = p;
        try {
            if (coopa::yaml::document_exists(p)) anim_clip_.model = ClipModel::from_node(coopa::yaml::load_document(p));
            else anim_clip_.model.name = state;
        } catch (const std::exception& e) {
            log_error("Timeline: " + std::string(e.what()));
        }
        anim_sel_keys_.clear();
        anim_runtime_rev_ = 0;
        anim_rename_buf_.clear();
        anim_view_ = {};
        return;
    }
}

void EditorApp::anim_clip_edit_(const std::string& label, const std::function<void(ClipModel&)>& fn, const std::string& merge_key) {
    if (!anim_clip_.open()) return;
    ClipModel before = anim_clip_.model;
    fn(anim_clip_.model);
    if (anim_clip_.model == before) return;
    anim_clip_.undo.push(label, std::move(before), anim_clip_.model, merge_key);
    ++anim_clip_.revision;
    anim_clip_dirty_ = true;
}

void EditorApp::anim_save_clip_() {
    anim_clip_dirty_ = false;
    try {
        fs::create_directories(anim_clip_.path.parent_path());
        coopa::yaml::save_document(anim_clip_.path, anim_clip_.model.to_node());
    } catch (const std::exception& e) {
        log_error("Timeline: could not save " + anim_clip_.path.string() + ": " + e.what());
    }
    ++anim_clip_.revision;
}

void EditorApp::anim_add_animator_(ObjectId id) {
    Node comp = Node::mapping();
    comp["type"] = Node(std::string("Animator"));
    comp["auto_play"] = Node(std::string(""));
    comp["states"] = Node::sequence();
    apply_(doc_.add_component(id, comp));
    anim_rig_ = id;
}

void EditorApp::anim_new_clip_(std::string name) {
    if (!anim_rig_) return;
    const auto states = anim_states_();
    auto taken = [&](const std::string& n) {
        for (const auto& s : states) if (s.first == n) return true;
        return false;
    };
    const std::string base = name;
    for (int i = 1; taken(name); ++i) {   // clip, clip_001, clip_002... (snake_case, like assets/)
        char buf[16];
        std::snprintf(buf, sizeof(buf), "_%03d", i);
        name = base + buf;
    }
    const std::string rig_name = anim_name_of_(anim_rig_, "Rig");
    const std::string rel = "animations/" + rig_name + "/" + name + ".yaml";
    ClipModel m;
    m.name = name;
    try {
        fs::create_directories((anim_new_clip_base_() / rel).parent_path());
        coopa::yaml::save_document(anim_new_clip_base_() / rel, m.to_node());
    } catch (const std::exception& e) {
        log_error("Timeline: " + std::string(e.what()));
        return;
    }
    const int ci = doc_.find_component(anim_rig_, "Animator");
    Node comp = doc_.find(anim_rig_)->at("components").as_seq()[static_cast<size_t>(ci)];
    if (!comp.contains("states") || !comp.at("states").is_sequence()) comp["states"] = Node::sequence();
    Node st = Node::mapping();
    st["name"] = Node(name);
    st["clip"] = Node(rel);
    comp["states"].as_seq().push_back(st);
    if (get_string(comp, "auto_play").empty()) comp["auto_play"] = Node(name);
    apply_(doc_.set_component(anim_rig_, ci, comp, "New Clip"));
    anim_open_clip_(name);
    anim_time_ = 0.0f;
    log_info("Timeline: new clip " + rel);
}

void EditorApp::anim_delete_clip_() {
    if (!anim_rig_ || !anim_clip_.open()) return;
    const int ci = doc_.find_component(anim_rig_, "Animator");
    if (ci < 0) return;
    Node comp = doc_.find(anim_rig_)->at("components").as_seq()[static_cast<size_t>(ci)];
    Node kept = Node::sequence();
    if (comp.contains("states")) {
        for (const auto& s : comp.at("states").as_seq()) if (get_string(s, "name") != anim_clip_.state) kept.as_seq().push_back(s);
    }
    comp["states"] = kept;
    if (get_string(comp, "auto_play") == anim_clip_.state) {
        comp["auto_play"] = Node(kept.as_seq().empty() ? std::string("") : get_string(kept.as_seq().front(), "name"));
    }
    std::error_code ec;
    fs::remove(anim_clip_.path, ec);
    log_info("Timeline: deleted clip " + anim_clip_.state);
    apply_(doc_.set_component(anim_rig_, ci, comp, "Delete Clip"));
    anim_clip_ = ClipDocument{};
    anim_sel_keys_.clear();
}

bool EditorApp::get_object_transform_(ObjectId id, glm::vec3& p, glm::vec3& r, glm::vec3& s) const {
    if (anim_live_edit_(id)) {
        if (auto* live = sync_.live(id); live && live->get_transform()) {
            const auto& t = live->get_transform()->transform();
            p = t.position();
            r = t.rotation_degrees();
            s = t.scale();
            return true;
        }
    }
    // An inherited child's document node holds only its overrides: show the merged pose.
    if (doc_.is_inherited(id)) return instance_transform_(id, p, r, s);
    return doc_.get_transform(id, p, r, s);
}

void EditorApp::set_object_transform_(ObjectId id, const glm::vec3& p, const glm::vec3& r, const glm::vec3& s, const std::string& label,
                           const std::string& merge_key) {
    if (anim_live_edit_(id)) {
        if (auto* live = sync_.live(id); live && live->get_transform()) {
            auto& t = live->get_transform()->transform();
            const glm::vec3 op = t.position(), orr = t.rotation_degrees(), os = t.scale();
            t.set_position(p);
            t.set_rotation(r);
            t.set_scale(s);
            // Auto-key: the channels this edit changed, at the playhead.
            const bool dp = glm::distance(op, p) > 1e-6f, dr = glm::distance(orr, r) > 1e-4f, ds = glm::distance(os, s) > 1e-6f;
            if ((dp || dr || ds) && anim_clip_.open()) {
                const std::string path = rig_path_of_(anim_rig_, id);
                const glm::quat q = t.rotation_quat();
                const float time = anim_time_;
                anim_clip_edit_("Auto Key", [&](ClipModel& m) {
                    if (dp) m.set_key(path, "position", time, glm::vec4(p, 0.0f));
                    if (dr) m.set_key(path, "rotation_quat", time, glm::vec4(q.x, q.y, q.z, q.w));
                    if (ds) m.set_key(path, "scale", time, glm::vec4(s, 0.0f));
                    if (time > m.length) m.length = time;
                }, "autokey");
                anim_pose_rev_ = anim_clip_.revision;   // this pose IS the key: no re-pose needed
                anim_runtime_rev_ = 0;                  // ...but the sampler must see the new keys later
            }
        }
        return;
    }
    if (doc_.is_inherited(id)) { set_inherited_transform_(id, p, r, s, label, merge_key); return; }
    apply_(doc_.set_transform(id, p, r, s, label, merge_key));
}

void EditorApp::anim_restore_rest_pose_() {
    for (const auto& [id, depth] : rig_objects_(anim_rig_)) {
        glm::vec3 p, r, s;
        auto* live = sync_.live(id);
        if (!live || !live->get_transform() || !doc_.get_transform(id, p, r, s)) continue;
        auto& t = live->get_transform()->transform();
        t.set_position(p);
        t.set_rotation(r);
        t.set_scale(s);
    }
}

bool EditorApp::anim_preview_wanted_() const {
    return show_bottom_ && bottom_view_ == 1 && anim_rig_ && anim_clip_.open() && !anim_show_rest_ && !playing() &&
           (active_type_ == AssetType::Scene || active_type_ == AssetType::Object) && sync_.live(anim_rig_) != nullptr;
}

void EditorApp::timeline_frame_(float dt) {
    // Clip edits are written once a frame; an auto-key drag is one undo step until the mouse lets go.
    if (anim_clip_dirty_ && anim_clip_.open()) { anim_save_clip_(); anim_clip_dirty_ = false; }
    if (!ui().input().down[0]) anim_clip_.undo.end_merge();
    // Another document (scene / object asset): ids restart there, so nothing carries over.
    const fs::path doc_path = doc_.path();
    if (doc_path != anim_doc_path_) {
        anim_doc_path_ = doc_path;
        anim_rig_ = 0;
        anim_posed_ = false;   // the live objects it posed belong to the old document
        anim_record_ = false;
        anim_playing_ = false;
        anim_time_ = 0.0f;     // the playhead too: a new document's rig opens at its rest frame
        anim_clip_ = ClipDocument{};
        anim_sel_keys_.clear();
    }
    // The rig: the selection's, kept while the selection has none (clicking around the scene).
    if (anim_rig_ && !doc_.find(anim_rig_)) { anim_rig_ = 0; anim_posed_ = false; anim_clip_ = ClipDocument{}; }
    if (const ObjectId sel = doc_.primary()) {
        const ObjectId r = rig_of_(sel);
        if (r && r != anim_rig_) {
            if (anim_posed_) { anim_restore_rest_pose_(); anim_posed_ = false; }
            anim_rig_ = r;
            anim_clip_ = ClipDocument{};
            anim_record_ = false;
        }
    }
    if (anim_rig_ && doc_.find_component(anim_rig_, "Animator") < 0) {
        if (anim_posed_) { anim_restore_rest_pose_(); anim_posed_ = false; }
        anim_clip_ = ClipDocument{};
    }
    // The open clip must still be one of the rig's states.
    if (anim_clip_.open()) {
        bool still = false;
        for (const auto& s : anim_states_()) still |= s.first == anim_clip_.state;
        if (!still) anim_clip_ = ClipDocument{};
    }
    if (!anim_clip_.open() && anim_rig_) {
        const auto states = anim_states_();
        if (!states.empty()) anim_open_clip_(states.front().first);
    }
    if (anim_playing_ && anim_clip_.open()) {
        anim_time_ += dt;
        const float len = std::max(anim_clip_.model.length, 1e-3f);
        if (anim_time_ > len) {
            if (anim_clip_.model.wrap == "once") { anim_time_ = len; anim_playing_ = false; }
            else anim_time_ = std::fmod(anim_time_, len);
        }
    }
    if (!anim_preview_wanted_()) {
        if (anim_posed_) { anim_restore_rest_pose_(); anim_posed_ = false; }
        anim_playing_ = false;
        return;
    }
    if (anim_runtime_rev_ != anim_clip_.revision || !anim_runtime_) {
        anim_runtime_ = std::make_shared<coopa::anim::AnimationClip>(coopa::anim::parse_clip(anim_clip_.model.to_node()));
        anim_runtime_rev_ = anim_clip_.revision;
        anim_pose_time_ = -1.0f;
    }
    // Recording keeps hand-posed (unkeyed) changes until the playhead or the keys move;
    // otherwise the clip drives the pose every frame (a document edit cannot unpose it).
    const bool changed = anim_pose_time_ != anim_time_ || anim_pose_rev_ != anim_runtime_rev_ || !anim_posed_;
    if (!anim_record_ || changed) {
        if (!anim_posed_) anim_restore_rest_pose_();   // unkeyed channels start from rest
        apply_clip_pose(*anim_runtime_, anim_time_, sync_.live(anim_rig_));
        anim_pose_time_ = anim_time_;
        anim_pose_rev_ = anim_runtime_rev_;
    }
    anim_posed_ = true;
}

void EditorApp::anim_insert_keys_() {
    if (!anim_rig_) return;
    if (!anim_clip_.open()) {
        if (anim_states_().empty()) anim_new_clip_();
        else anim_open_clip_(anim_states_().front().first);
    }
    if (!anim_clip_.open()) return;
    std::vector<ObjectId> targets;
    for (ObjectId id : doc_.selection()) if (in_anim_rig_(id)) targets.push_back(id);
    if (targets.empty()) targets.push_back(anim_rig_);
    const float t = anim_time_;
    struct Pose { std::string path; glm::vec3 p; glm::quat q; glm::vec3 s; };
    std::vector<Pose> poses;
    for (ObjectId id : targets) {
        auto* live = sync_.live(id);
        if (!live || !live->get_transform()) continue;
        const auto& tr = live->get_transform()->transform();
        poses.push_back({rig_path_of_(anim_rig_, id), tr.position(), tr.rotation_quat(), tr.scale()});
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
    anim_pose_rev_ = 0;   // re-pose from the new keys (still the same pose at this frame)
}

void EditorApp::anim_delete_selected_keys_() {
    if (anim_sel_keys_.empty()) return;
    const auto refs = anim_sel_keys_;
    anim_clip_edit_("Delete Keyframes", [&](ClipModel& m) { m.delete_keys(refs); });
    anim_sel_keys_.clear();
}

void EditorApp::anim_key_channel_(ObjectId id, const std::string& property) {
    if (!anim_clip_.open() || !in_anim_rig_(id)) return;
    auto* live = sync_.live(id);
    if (!live || !live->get_transform()) return;
    const auto& tr = live->get_transform()->transform();
    glm::vec4 v;
    if (property == "position") v = glm::vec4(tr.position(), 0.0f);
    else if (property == "scale") v = glm::vec4(tr.scale(), 0.0f);
    else { const glm::quat q = tr.rotation_quat(); v = glm::vec4(q.x, q.y, q.z, q.w); }
    const std::string path = rig_path_of_(anim_rig_, id);
    const float t = anim_time_;
    anim_clip_edit_("Insert Keyframe", [&](ClipModel& m) {
        m.set_key(path, property, t, v);
        if (t > m.length) m.length = t;
    });
}

bool EditorApp::anim_keyed_now_(ObjectId id, const std::string& property) const {
    if (!anim_clip_.open() || !in_anim_rig_(id)) return false;
    for (float k : anim_clip_.model.key_times(rig_path_of_(anim_rig_, id), property)) {
        if (std::abs(k - anim_time_) <= 0.5f / kAnimFps) return true;
    }
    return false;
}

void EditorApp::anim_rename_clip_(const std::string& to) {
    if (!anim_rig_ || !anim_clip_.open() || to.empty() || to == anim_clip_.state) return;
    for (const auto& s : anim_states_()) if (s.first == to) { log_warn("Timeline: a clip named " + to + " exists"); return; }
    const int ci = doc_.find_component(anim_rig_, "Animator");
    if (ci < 0) return;
    Node comp = doc_.find(anim_rig_)->at("components").as_seq()[static_cast<size_t>(ci)];
    const fs::path old_path = anim_clip_.path;
    const fs::path new_path = old_path.parent_path() / (to + ".yaml");
    std::string new_rel;
    for (auto& st : comp["states"].as_seq()) {
        if (get_string(st, "name") != anim_clip_.state) continue;
        st["name"] = Node(to);
        const std::string rel = get_string(st, "clip");
        new_rel = rel.substr(0, rel.find_last_of('/') + 1) + to + ".yaml";
        st["clip"] = Node(new_rel);
    }
    if (get_string(comp, "auto_play") == anim_clip_.state) comp["auto_play"] = Node(to);
    std::error_code ec;
    fs::rename(old_path, new_path, ec);
    if (ec) { log_error("Timeline: could not rename the clip file: " + ec.message()); return; }
    apply_(doc_.set_component(anim_rig_, ci, comp, "Rename Clip"));
    anim_clip_.state = to;
    anim_clip_.path = new_path;
    anim_clip_.model.name = to;
    anim_save_clip_();
    log_info("Timeline: renamed clip to " + to);
}

void EditorApp::draw_timeline_(imm::Context& ctx, const imm::Box& hb, const imm::Box& body, float x) {
    using I = imm::Icon;
    const float bh = hb.h - 6;
    timeline_hovered_ = ctx.is_hovered(body) || ctx.is_hovered(hb);
    if (!anim_rig_) {
        const ObjectId sel = doc_.primary();
        // A placed object asset: its rig and clips live in the asset -- animate it there.
        if (const std::string inst = sel ? anim_instance_asset_(sel) : std::string(); !inst.empty()) {
            ctx.text_in({x, hb.y, 330, hb.h}, "Placed from " + inst + ": animate it in its object asset", ctx.style.text_dim, 0.0f);
            ctx.push_id("openinst");
            const imm::Box b{x + 330, hb.y + 3, 140, bh};
            bool hov = false, held = false;
            if (ctx.invisible_button("open", b, &hov, &held)) {
                const fs::path p = project_.assets() / (inst + (fs::path(inst).has_extension() ? "" : ".yaml"));
                open_object_asset(p);
            }
            ctx.fill_rounded(b, hov ? ctx.style.button_hover : ctx.style.button);
            ctx.text_in(b, "Open Object Asset", ctx.style.text, 0.0f, true);
            ctx.tooltip("Open " + inst + "\nIts Timeline edits the clips every placed copy plays");
            ctx.pop_id();
            return;
        }
        ctx.text_in({x, hb.y, 360, hb.h}, sel ? "No Animator on this object or its parents" : "Select an object to animate",
                    ctx.style.text_dim, 0.0f);
        if (sel && (active_type_ == AssetType::Scene || active_type_ == AssetType::Object)) {
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
    const std::string rig_name = anim_name_of_(anim_rig_, "Rig");
    // Clip dropdown.
    const std::string clip_label = anim_clip_.open() ? anim_clip_.state : std::string("No Clip");
    const float cw = std::max(110.0f, ctx.text_width(clip_label) + bh + 20);
    {
        const imm::Box b{x, hb.y + 3, cw, bh};
        bool hov = false, held = false;
        if (ctx.invisible_button("clip_dd", b, &hov, &held)) ctx.open_popup("clip_menu", glm::vec2(b.x, b.bottom() + 2));
        ctx.fill_rounded(b, hov ? ctx.style.button_hover : ctx.style.button);
        ctx.icon(I::Play, {b.x + 3, b.y + 2, bh - 4, bh - 4}, ctx.style.text_dim);
        ctx.text_in({b.x + bh + 2, b.y, cw - bh - 16, bh}, clip_label, ctx.style.text, 0.0f);
        ctx.arrow({b.right() - 14, b.y + 4, 10, bh - 8}, true, ctx.style.text_dim);
        ctx.tooltip("Clip\n" + rig_name + "'s clips (its Animator's states). Each is a file under animations/" + rig_name + "/");
        if (ctx.begin_popup("clip_menu", 200)) {
            for (const auto& [state, path] : anim_states_()) {
                bool on = state == anim_clip_.state;
                if (ctx.menu_item(state, "", &on)) { anim_open_clip_(state); anim_time_ = 0.0f; }
            }
            ctx.menu_separator();
            if (ctx.menu_item("New Clip", "", nullptr, true, I::Plus)) anim_new_clip_();
            if (anim_clip_.open()) {
                if (anim_rename_buf_.empty()) anim_rename_buf_ = anim_clip_.state;
                if (ctx.input_text("Rename", &anim_rename_buf_)) anim_rename_clip_(anim_rename_buf_);
            }
            if (ctx.menu_item("Delete Clip", "", nullptr, anim_clip_.open(), I::Trash)) anim_delete_clip_();
            ctx.end_popup();
        }
        x += cw + 6;
    }
    if (!anim_clip_.open()) {
        ctx.text_in({x, hb.y, 260, hb.h}, "Rig " + rig_name + ": add a clip to start animating", ctx.style.text_dim, 0.0f);
        return;
    }
    auto icon_btn = [&](const char* id, I ic, const char* tip, bool on) {
        const bool r = ctx.icon_button(id, ic, tip, on, bh, imm::Context::kAll, imm::Box{x, hb.y + 3, bh, bh});
        x += bh + 3;
        return r;
    };
    if (icon_btn("tl_start", I::Undo, "Jump to Start\nHome", false)) anim_time_ = 0.0f;
    if (icon_btn("tl_play", anim_playing_ ? I::Pause : I::Play, "Play / Pause\nSpace (in the Timeline)", anim_playing_)) {
        anim_playing_ = !anim_playing_;
    }
    x += 4;
    float frame = std::round(anim_time_ * kAnimFps);
    ctx.drag_float_box("tl_frame", {x, hb.y + 3, 56, bh}, &frame, 0.2f, 0.0f, 100000.0f, "%.0f");
    ctx.tooltip("Frame\nThe playhead (30 frames a second)");
    anim_time_ = frame / kAnimFps;
    x += 62;
    // Record: rig transforms change the live pose only.
    {
        const imm::Box b{x, hb.y + 3, bh, bh};
        bool hov = false, held = false;
        if (ctx.invisible_button("tl_rec", b, &hov, &held)) anim_record_ = !anim_record_;
        ctx.fill_rounded(b, anim_record_ ? glm::vec4(0.55f, 0.12f, 0.12f, 1) : hov ? ctx.style.button_hover : ctx.style.button);
        ctx.circle(b.center(), bh * 0.28f, anim_record_ ? glm::vec4(1, 0.3f, 0.3f, 1) : glm::vec4(0.85f, 0.25f, 0.25f, 1));
        ctx.tooltip("Record\nPose the rig for keys: moving its objects changes only the animated pose, never the rest pose. "
                    "I keys the selection.");
        x += bh + 3;
    }
    if (icon_btn("tl_key", I::Plus, "Insert Keyframe\nI: key position, rotation and scale of the selected rig objects", false)) {
        anim_insert_keys_();
    }
    if (icon_btn("tl_rest", I::ObjectMode, "Rest Pose\nShow the rig as authored instead of the clip", anim_show_rest_)) {
        anim_show_rest_ = !anim_show_rest_;
    }
    if (icon_btn("tl_all", I::Collection, "All Objects\nList every object of the rig (off: the animated and selected ones)",
                 anim_show_all_)) {
        anim_show_all_ = !anim_show_all_;
    }
    x += 6;
    ctx.text_in({x, hb.y, 44, hb.h}, "Length", ctx.style.text_dim, 0.0f);
    x += 44;
    float len_frames = std::round(anim_clip_.model.length * kAnimFps);
    if (ctx.drag_float_box("tl_len", {x, hb.y + 3, 52, bh}, &len_frames, 0.2f, 1.0f, 100000.0f, "%.0f")) {
        const float l = len_frames / kAnimFps;
        anim_clip_edit_("Clip Length", [l](ClipModel& m) { m.length = l; });
    }
    ctx.tooltip("Length\nThe clip's length in frames");
    x += 58;
    {
        static const char* kWraps[] = {"loop", "once", "pingpong"};
        int w = 0;
        for (int i = 0; i < 3; ++i) if (anim_clip_.model.wrap == kWraps[i]) w = i;
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

std::string EditorApp::anim_channel_label_(const std::string& property) {
    if (property == "position") return "Position";
    if (property == "rotation_quat" || property == "rotation") return "Rotation";
    if (property == "scale") return "Scale";
    return property;
}

void EditorApp::draw_dope_sheet_(imm::Context& ctx, const imm::Box& body) {
    const auto& in = ctx.input();
    const float name_w = 190.0f, ruler_h = 20.0f, row_h = 20.0f;
    const imm::Box names{body.x, body.y, name_w, body.h};
    const imm::Box track{body.x + name_w, body.y, body.w - name_w, body.h};
    ctx.fill(body, ctx.style.panel_bg);
    ctx.fill(names, ctx.style.panel_alt);
    const float len = std::max(anim_clip_.model.length, 1.0f / kAnimFps);
    // Visible range (seconds): fit the clip (plus a little) until zoomed or panned.
    if (anim_view_.end <= anim_view_.start) anim_view_ = {0.0f, len * 1.04f + 2.0f / kAnimFps};
    const float x0 = track.x + 12, x1 = track.right() - 12;
    const float pps = (x1 - x0) / std::max(anim_view_.end - anim_view_.start, 1e-3f);
    auto x_of = [&](float t) { return x0 + (t - anim_view_.start) * pps; };
    auto t_of = [&](float x) { return anim_view_.start + (x - x0) / pps; };
    const glm::vec2 m = ctx.mouse();

    // Rows scroll with the wheel over the names column.
    if (ctx.is_hovered(names) && in.scroll.y != 0.0f && !ctx.any_popup_open()) anim_row_scroll_ -= static_cast<int>(in.scroll.y);
    // Zoom (wheel, about the mouse) and pan (Shift-wheel, middle-drag).
    if (ctx.is_hovered(track) && !ctx.any_popup_open()) {
        if (in.scroll.y != 0.0f && !has(in.mods, Mods::Shift)) {
            const float at = t_of(m.x);
            const float k = std::pow(0.85f, in.scroll.y);
            anim_view_.start = at - (at - anim_view_.start) * k;
            anim_view_.end = at + (anim_view_.end - at) * k;
        } else if (in.scroll.y != 0.0f || in.scroll.x != 0.0f) {
            const float d = -(in.scroll.y + in.scroll.x) * 30.0f / pps;
            anim_view_.start += d;
            anim_view_.end += d;
        }
        if (in.down[2] && in.mouse_delta.x != 0.0f) {
            anim_view_.start -= in.mouse_delta.x / pps;
            anim_view_.end -= in.mouse_delta.x / pps;
        }
    }

    // Ruler.
    const imm::Box ruler{track.x, track.y, track.w, ruler_h};
    ctx.fill(ruler, ctx.style.header);
    ctx.push_clip(track);
    int label_every = 1;
    while (label_every * pps / kAnimFps < 34.0f) label_every *= (label_every == 1 ? 5 : 2);
    const int f0 = static_cast<int>(std::floor(anim_view_.start * kAnimFps)), f1 = static_cast<int>(std::ceil(anim_view_.end * kAnimFps));
    for (int f = std::max(f0, -1000000); f <= f1; ++f) {
        const float x = x_of(f / kAnimFps);
        const bool major = f % label_every == 0;
        if (!major && pps / kAnimFps < 5.0f) continue;
        ctx.line({x, ruler.bottom() - (major ? 7.0f : 3.0f)}, {x, ruler.bottom()}, ctx.style.text_dim, 1.0f);
        if (major) ctx.draw_text({x + 2, ruler.y + 2}, std::to_string(f), ctx.style.text_dim, 10.0f);
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
    for (const auto& t : anim_clip_.model.tracks) animated.insert(t.object);
    for (const auto& [id, depth] : rig_objects_(anim_rig_)) {
        const std::string path = rig_path_of_(anim_rig_, id);
        if (!anim_show_all_ && id != anim_rig_ && !animated.count(path) && !doc_.is_selected(id)) continue;
        rows.push_back({anim_name_of_(id), path, "", false, id, anim_show_all_ ? depth : (id == anim_rig_ ? 0 : 1)});
        if (!anim_expanded_.count(path)) continue;
        std::vector<std::string> props;
        for (const auto& t : anim_clip_.model.tracks) {
            if (t.object == path && !t.is_procedural() && std::find(props.begin(), props.end(), t.property) == props.end()) props.push_back(t.property);
        }
        for (const auto& prop : props) rows.push_back({anim_channel_label_(prop), path, prop, false, id, depth + 1});
    }
    auto row_times = [&](const Row& r) {
        if (r.events) return std::vector<float>{};
        if (r.summary) return anim_clip_.model.key_times();
        if (!r.property.empty()) return anim_clip_.model.key_times(r.path, r.property);
        return anim_clip_.model.key_times(&r.path);
    };
    // The dope-sheet cells a diamond stands for.
    auto cells_of = [&](const Row& r, float t) {
        std::set<ClipModel::KeyRef> cells;
        if (r.summary) {
            for (const auto& rw : rows) {
                if (rw.summary || rw.events || !rw.property.empty()) continue;
                for (float kt : anim_clip_.model.key_times(&rw.path)) if (std::abs(kt - t) <= ClipModel::kTimeEps) cells.insert({rw.path, kt, ""});
            }
        } else {
            cells.insert({r.path, t, r.property});
        }
        return cells;
    };
    auto is_selected = [&](const Row& r, float t) {
        for (const auto& k : anim_sel_keys_) {
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
    anim_row_scroll_ = std::clamp(anim_row_scroll_, 0, std::max(0, static_cast<int>(rows.size()) - kPinned - visible));
    if (anim_row_scroll_ > 0) rows.erase(rows.begin() + kPinned, rows.begin() + kPinned + anim_row_scroll_);
    if (static_cast<int>(rows.size()) - kPinned > visible || anim_row_scroll_ > 0) {
        // A thin scrollbar on the names column's right edge.
        const float total = static_cast<float>(rows.size() - kPinned + anim_row_scroll_);
        const float h = body.h - ruler_h;
        ctx.fill_rounded({names.right() - 4, body.y + ruler_h + h * anim_row_scroll_ / total, 3, h * visible / total},
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
        const bool selected = !row.summary && row.property.empty() && doc_.is_selected(row.id);
        if (selected) ctx.fill({names.x, y, names.w, row_h}, imm::with_alpha(ctx.style.selection, 0.6f));
        float tx = names.x + 6 + row.depth * 12.0f;
        // The expand arrow (objects with tracks).
        if (!row.summary && row.property.empty()) {
            bool has_tracks = false;
            for (const auto& t : anim_clip_.model.tracks) has_tracks |= t.object == row.path && !t.is_procedural();
            if (has_tracks) {
                const imm::Box ab{tx, y + 4, 12, row_h - 8};
                const bool open = anim_expanded_.count(row.path) > 0;
                ctx.arrow(ab, open, ctx.style.text_dim);
                if (ctx.is_hovered(ab) && in.pressed[0]) {
                    if (open) anim_expanded_.erase(row.path); else anim_expanded_.insert(row.path);
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
            doc_.select(row.id, has(in.mods, Mods::Shift));
        }
        ctx.push_clip(track);
        for (float t : row_times(row)) {
            const bool sel_key = is_selected(row, t);
            const glm::vec2 c{x_of(t) + (anim_drag_.active && sel_key ? anim_drag_.dt * pps : 0.0f), y + row_h * 0.5f};
            const float rr = row.summary ? 5.5f : !row.property.empty() ? 4.0f : 4.5f;
            const glm::vec4 fillc = sel_key ? glm::vec4(1.0f, 0.62f, 0.18f, 1) : row.summary ? glm::vec4(0.95f, 0.95f, 0.95f, 1)
                                                                                : glm::vec4(0.78f, 0.78f, 0.78f, 1);
            ctx.triangle({c.x, c.y - rr}, {c.x + rr, c.y}, {c.x, c.y + rr}, fillc);
            ctx.triangle({c.x, c.y - rr}, {c.x, c.y + rr}, {c.x - rr, c.y}, fillc);
            if (!in_track || on_key || glm::distance(m, c) > rr + 2.0f) continue;
            if (in.pressed[0]) {
                on_key = true;
                anim_sel_event_ = -1;
                const auto cells = cells_of(row, t);
                if (has(in.mods, Mods::Shift)) {
                    for (const auto& c2 : cells) {
                        if (anim_sel_keys_.count(c2)) anim_sel_keys_.erase(c2);
                        else anim_sel_keys_.insert(c2);
                    }
                } else if (!sel_key) {
                    anim_sel_keys_ = cells;
                }
                anim_drag_ = {true, m.x, 0.0f};
            } else if (in.pressed[1]) {
                on_key = true;
                if (!sel_key) anim_sel_keys_ = cells_of(row, t);
                anim_menu_time_ = t;
                ctx.open_popup("key_menu", m);
            }
        }
        ctx.pop_clip();
    }
    ctx.pop_clip();

    // Dragging keys: snapped to frames, applied as one edit on release.
    if (anim_drag_.active) {
        if (in.down[0]) {
            anim_drag_.dt = std::round((m.x - anim_drag_.start_x) / pps * kAnimFps) / kAnimFps;
        } else {
            const float d = anim_drag_.dt;
            anim_drag_.active = false;
            if (std::abs(d) > 1e-6f) {
                const auto refs = anim_sel_keys_;
                std::set<ClipModel::KeyRef> moved;
                anim_clip_edit_("Move Keyframes", [&](ClipModel& mm) { moved = mm.move_keys(refs, d); });
                anim_sel_keys_ = moved;
            }
        }
    } else if (in_track && !on_key) {
        if (in.pressed[0]) { anim_sel_keys_.clear(); anim_sel_event_ = -1; }   // empty space
        if (in.pressed[1]) {
            anim_menu_time_ = anim_snap_(t_of(m.x));
            ctx.open_popup("track_menu", m);
        }
    }

    // Dragging an event flag: snapped to frames, one edit on release.
    if (anim_event_drag_.active) {
        if (in.down[0]) {
            anim_event_drag_.dt = std::round((m.x - anim_event_drag_.start_x) / pps * kAnimFps) / kAnimFps;
        } else {
            const float d = anim_event_drag_.dt;
            anim_event_drag_.active = false;
            if (std::abs(d) > 1e-6f && anim_sel_event_ >= 0 && anim_sel_event_ < static_cast<int>(anim_clip_.model.events.size())) {
                const size_t idx = static_cast<size_t>(anim_sel_event_);
                const float to = anim_snap_(anim_clip_.model.events[idx].time + d);
                int moved = anim_sel_event_;
                anim_clip_edit_("Move Event", [&](ClipModel& mm) { moved = mm.move_event(idx, to); });
                anim_sel_event_ = moved;
            }
        }
    }

    // Right-click menus.
    if (ctx.begin_popup("events_lane_menu", 190)) {
        if (ctx.menu_item("Add Event Here", "", nullptr, true, imm::Icon::Plus)) anim_add_event_(anim_menu_time_, "event");
        ctx.end_popup();
    }
    if (ctx.begin_popup("event_menu", 230)) {
        if (anim_sel_event_ >= 0 && anim_sel_event_ < static_cast<int>(anim_clip_.model.events.size())) {
            const size_t idx = static_cast<size_t>(anim_sel_event_);
            const ClipEvent ev = anim_clip_.model.events[idx];
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
            if (ctx.menu_item("Jump to Event", "")) anim_time_ = ev.time;
            if (ctx.menu_item("Delete Event", "X", nullptr, true, imm::Icon::Trash)) anim_delete_selected_event_();
        }
        ctx.end_popup();
    }
    if (ctx.begin_popup("key_menu", 190)) {
        const std::string cur = anim_clip_.model.easing_of(anim_sel_keys_);
        ctx.label_dim("Interpolation");
        static const char* kEase[][2] = {{"linear", "Linear"}, {"step", "Constant (Step)"}, {"ease_in", "Ease In"},
                                         {"ease_out", "Ease Out"}, {"ease_in_out", "Ease In-Out"}};
        for (const auto& e : kEase) {
            bool on = cur == e[0];
            if (ctx.menu_item(e[1], "", &on)) {
                const auto refs = anim_sel_keys_;
                const std::string ease = e[0];
                anim_clip_edit_("Interpolation", [&](ClipModel& mm) { mm.set_easing(refs, ease); });
            }
        }
        ctx.menu_separator();
        if (ctx.menu_item("Jump to Key", "")) anim_time_ = anim_menu_time_;
        if (ctx.menu_item("Delete Keyframes", "X", nullptr, true, imm::Icon::Trash)) anim_delete_selected_keys_();
        ctx.end_popup();
    }
    if (ctx.begin_popup("track_menu", 190)) {
        if (ctx.menu_item("Insert Keyframe Here", "I")) { anim_time_ = anim_menu_time_; anim_insert_keys_(); }
        if (ctx.menu_item("Select All Keys", "A")) anim_select_all_keys_();
        if (ctx.menu_item("Frame All", "Home")) anim_view_ = {};
        ctx.end_popup();
    }

    // Scrubbing on the ruler.
    if ((ctx.is_hovered(ruler) && in.pressed[0]) || anim_scrubbing_) {
        anim_scrubbing_ = in.down[0];
        const float t = std::max(0.0f, t_of(m.x));
        anim_time_ = has(in.mods, Mods::Shift) ? t : anim_snap_(t);
    }
    // The playhead.
    ctx.push_clip(track);
    const float px = x_of(anim_time_);
    ctx.line({px, track.y}, {px, track.bottom()}, glm::vec4(0.32f, 0.55f, 0.95f, 1), 2.0f);
    const imm::Box tag{px - 15, ruler.y + 1, 30, ruler_h - 2};
    ctx.fill_rounded(tag, glm::vec4(0.32f, 0.55f, 0.95f, 1), 3);
    ctx.text_in(tag, std::to_string(static_cast<int>(std::round(anim_time_ * kAnimFps))), glm::vec4(1), 0.0f, true);
    ctx.pop_clip();

    // Keys over the panel.
    if (timeline_hovered_ && !ctx.wants_keyboard() && !ctx.any_popup_open()) {
        if (ctx.shortcut(Key::Space)) anim_playing_ = !anim_playing_;
        if (ctx.shortcut(Key::I)) anim_insert_keys_();
        if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete)) {
            if (anim_sel_event_ >= 0) anim_delete_selected_event_();
            else anim_delete_selected_keys_();
        }
        if (ctx.shortcut(Key::A)) anim_select_all_keys_();
        if (ctx.shortcut(Key::Right)) anim_time_ = anim_snap_(anim_time_ + 1.0f / kAnimFps);
        if (ctx.shortcut(Key::Left)) anim_time_ = anim_snap_(anim_time_ - 1.0f / kAnimFps);
        if (ctx.shortcut(Key::Up)) anim_time_ = anim_clip_.model.neighbour_key(anim_time_, +1);
        if (ctx.shortcut(Key::Down)) anim_time_ = anim_clip_.model.neighbour_key(anim_time_, -1);
        if (ctx.shortcut(Key::Home)) { anim_time_ = 0.0f; anim_view_ = {}; }
    }
}

void EditorApp::anim_add_event_(float time, const std::string& name) {
    if (!anim_clip_.open()) return;
    size_t at = 0;
    anim_clip_edit_("Add Event", [&](ClipModel& m) {
        at = m.add_event(time, name);
        if (time > m.length) m.length = time;
    });
    anim_sel_event_ = static_cast<int>(at);
    anim_sel_keys_.clear();
}

void EditorApp::anim_delete_selected_event_() {
    if (anim_sel_event_ < 0) return;
    const size_t idx = static_cast<size_t>(anim_sel_event_);
    anim_clip_edit_("Delete Event", [&](ClipModel& m) { m.delete_event(idx); });
    anim_sel_event_ = -1;
}

void EditorApp::anim_select_all_keys_() {
    anim_sel_keys_.clear();
    anim_sel_event_ = -1;
    for (const auto& t : anim_clip_.model.tracks) for (const auto& k : t.keys) anim_sel_keys_.insert({t.object, k.time, ""});
}

void EditorApp::show_timeline() { show_bottom_ = true; bottom_view_ = 1; }

void EditorApp::set_selected_key_interpolation(const std::string& ease) {
    const auto refs = anim_sel_keys_;
    anim_clip_edit_("Interpolation", [&](ClipModel& m) { m.set_easing(refs, ease); });
}

void EditorApp::move_selected_animation_event(float to_time) {
    if (anim_sel_event_ < 0 || anim_sel_event_ >= static_cast<int>(anim_clip_.model.events.size())) return;
    const size_t idx = static_cast<size_t>(anim_sel_event_);
    int moved = anim_sel_event_;
    anim_clip_edit_("Move Event", [&](ClipModel& m) { moved = m.move_event(idx, anim_snap_(to_time)); });
    anim_sel_event_ = moved;
}

void EditorApp::rename_selected_animation_event(const std::string& name) {
    if (anim_sel_event_ < 0 || anim_sel_event_ >= static_cast<int>(anim_clip_.model.events.size()) || name.empty()) return;
    const size_t idx = static_cast<size_t>(anim_sel_event_);
    anim_clip_edit_("Rename Event", [&](ClipModel& m) { m.events[idx].name = name; });
}

} // namespace editor
} // namespace toy
