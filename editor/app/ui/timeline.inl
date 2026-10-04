// editor/app/ui/timeline.inl -- included inside EditorApp's class body.
//
// Animation: the Timeline (a dope sheet) in the bottom area, next to the Console.
//
// A rig is an object hierarchy whose root carries an Animator (coopa::anim). Selecting the root
// or anything under it makes it the Timeline's rig. Its clips are the Animator's `states`, each a
// separate clip file under `<scene>/animations/<rig>/` -- created, listed and deleted from here
// only, never from the asset browser: a clip belongs to its object. The panel edits one clip at
// a time (a ClipModel, mesh-document style: own undo, written through to the file on every edit).
//
// Poses. The scene document holds the REST pose (what a skinned mesh binds against). While the
// Timeline shows a clip, the live rig is posed by it at the playhead (anim/clip_pose.h); leaving
// it (another tab, Rest, Play) puts the rest pose back. Record (the red dot) makes transforms of
// rig objects -- gizmo, G/R/S, the sidebar -- change the live pose only, so posing for a key never
// disturbs the rest pose; I keys position, rotation (quaternion) and scale of the selected rig
// objects at the playhead. Keys: click / Shift-click diamonds, drag to move (snapped to frames),
// X / Delete removes them; Space plays, arrows step frames.

    static constexpr float kAnimFps = 30.0f;   ///< The Timeline's frame grid (keys snap to it).

    /** @brief The clip being edited: its file, the model, and its own undo history. */
    struct ClipDocument {
        std::string state;        ///< The Animator state naming it.
        fs::path path;
        ClipModel model;
        UndoStack<ClipModel> undo{200};
        uint64_t revision = 1;    ///< Bumps on every change (the preview re-parses).
        bool open() const { return !path.empty(); }
    };
    struct KeyDrag { bool active = false; float start_x = 0.0f; float dt = 0.0f; };

    // =================================================================================
    // Rig and clip context
    // =================================================================================

    /** @brief The rig root `id` belongs to: itself or its nearest ancestor with an Animator (0 if none). */
    ObjectId rig_of_(ObjectId id) const {
        for (std::optional<ObjectId> cur = id; cur && *cur != 0; cur = doc_.parent_of(*cur)) {
            if (doc_.find_component(*cur, "Animator") >= 0) return *cur;
        }
        return 0;
    }

    /** @brief `id`'s track path from rig root `rig` ("" for the root, "Arm/Hand" below it). */
    std::string rig_path_of_(ObjectId rig, ObjectId id) const {
        std::vector<std::string> names;
        for (std::optional<ObjectId> cur = id; cur && *cur != 0 && *cur != rig; cur = doc_.parent_of(*cur)) {
            const Node* n = doc_.find(*cur);
            names.push_back(n ? get_string(*n, "name") : std::string());
        }
        std::string out;
        for (auto it = names.rbegin(); it != names.rend(); ++it) out += (out.empty() ? "" : "/") + *it;
        return out;
    }

    /** @brief The rig root and every object under it, depth first (the Timeline's rows). */
    std::vector<std::pair<ObjectId, int>> rig_objects_(ObjectId rig) const {
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

    bool in_anim_rig_(ObjectId id) const { return anim_rig_ && (id == anim_rig_ || doc_.is_ancestor(anim_rig_, id)); }

    fs::path anim_scene_dir_() const { return (doc_.path().empty() ? sync_.fallback_path : doc_.path()).parent_path(); }

    /** @brief The rig's Animator states: (state name, clip path as written). */
    std::vector<std::pair<std::string, std::string>> anim_states_() const {
        std::vector<std::pair<std::string, std::string>> out;
        if (!anim_rig_) return out;
        const int ci = doc_.find_component(anim_rig_, "Animator");
        if (ci < 0) return out;
        const Node& comp = doc_.find(anim_rig_)->at("components").as_seq()[static_cast<size_t>(ci)];
        if (!comp.contains("states") || !comp.at("states").is_sequence()) return out;
        for (const auto& s : comp.at("states").as_seq()) out.push_back({get_string(s, "name"), get_string(s, "clip")});
        return out;
    }

    /** @brief Opens state `name`'s clip file (creating an empty clip if the file is missing). */
    void anim_open_clip_(const std::string& name) {
        for (const auto& [state, clip] : anim_states_()) {
            if (state != name) continue;
            const fs::path p = anim_scene_dir_() / clip;
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
            return;
        }
    }

    /** @brief One undoable clip edit, written straight through to its file. */
    void anim_clip_edit_(const std::string& label, const std::function<void(ClipModel&)>& fn) {
        if (!anim_clip_.open()) return;
        ClipModel before = anim_clip_.model;
        fn(anim_clip_.model);
        if (anim_clip_.model == before) return;
        anim_clip_.undo.push(label, std::move(before), anim_clip_.model);
        anim_save_clip_();
    }

    void anim_save_clip_() {
        try {
            fs::create_directories(anim_clip_.path.parent_path());
            coopa::yaml::save_document(anim_clip_.path, anim_clip_.model.to_node());
        } catch (const std::exception& e) {
            log_error("Timeline: could not save " + anim_clip_.path.string() + ": " + e.what());
        }
        ++anim_clip_.revision;
    }

    /** @brief Adds an Animator to `id`: it becomes a rig root. */
    void anim_add_animator_(ObjectId id) {
        Node comp = Node::mapping();
        comp["type"] = Node(std::string("Animator"));
        comp["auto_play"] = Node(std::string(""));
        comp["states"] = Node::sequence();
        apply_(doc_.add_component(id, comp));
        anim_rig_ = id;
    }

    /** @brief A new clip on the rig: a file under animations/<rig>/, a state, and (first clip) auto_play. */
    void anim_new_clip_(std::string name = "Clip") {
        if (!anim_rig_) return;
        const auto states = anim_states_();
        auto taken = [&](const std::string& n) {
            for (const auto& s : states) if (s.first == n) return true;
            return false;
        };
        const std::string base = name;
        for (int i = 1; taken(name); ++i) name = base + "." + std::to_string(i);
        const std::string rig_name = get_string(*doc_.find(anim_rig_), "name", "Rig");
        const std::string rel = "animations/" + rig_name + "/" + name + ".yaml";
        ClipModel m;
        m.name = name;
        try {
            fs::create_directories((anim_scene_dir_() / rel).parent_path());
            coopa::yaml::save_document(anim_scene_dir_() / rel, m.to_node());
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

    /** @brief Removes the open clip's state from the Animator and deletes its file. */
    void anim_delete_clip_() {
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

    // =================================================================================
    // Poses
    // =================================================================================

    /** @brief Rig objects follow the live pose for transform edits (Record), not the document. */
    bool anim_live_edit_(ObjectId id) const { return anim_record_ && anim_posed_ && in_anim_rig_(id); }

    /** @brief An object's transform as edits should start from: the live pose while recording, else the document. */
    bool get_object_transform_(ObjectId id, glm::vec3& p, glm::vec3& r, glm::vec3& s) const {
        if (anim_live_edit_(id)) {
            if (auto* live = sync_.live(id); live && live->get_transform()) {
                const auto& t = live->get_transform()->transform();
                p = t.position();
                r = t.rotation_degrees();
                s = t.scale();
                return true;
            }
        }
        return doc_.get_transform(id, p, r, s);
    }

    /** @brief Sets an object's transform: on the live pose while recording a rig object, else as a document edit. */
    void set_object_transform_(ObjectId id, const glm::vec3& p, const glm::vec3& r, const glm::vec3& s, const std::string& label,
                               const std::string& merge_key = {}) {
        if (anim_live_edit_(id)) {
            if (auto* live = sync_.live(id); live && live->get_transform()) {
                auto& t = live->get_transform()->transform();
                t.set_position(p);
                t.set_rotation(r);
                t.set_scale(s);
            }
            return;
        }
        apply_(doc_.set_transform(id, p, r, s, label, merge_key));
    }

    /** @brief Puts the document's rest pose back on the live rig. */
    void anim_restore_rest_pose_() {
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

    /** @brief Should the live rig show the open clip at the playhead? */
    bool anim_preview_wanted_() const {
        return show_bottom_ && bottom_view_ == 1 && anim_rig_ && anim_clip_.open() && !anim_show_rest_ && !playing() &&
               active_type_ == AssetType::Scene && sync_.live(anim_rig_) != nullptr;
    }

    /** @brief Per frame (pre-render): follow the selection's rig, play, and pose the live rig. */
    void timeline_frame_(float dt) {
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

    // =================================================================================
    // Keys
    // =================================================================================

    static float anim_snap_(float t) { return std::max(0.0f, std::round(t * kAnimFps) / kAnimFps); }

    /** @brief I: keys position, rotation and scale of the selected rig objects (the root if none) at the playhead. */
    void anim_insert_keys_() {
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

    void anim_delete_selected_keys_() {
        if (anim_sel_keys_.empty()) return;
        const auto refs = anim_sel_keys_;
        anim_clip_edit_("Delete Keyframes", [&](ClipModel& m) { m.delete_keys(refs); });
        anim_sel_keys_.clear();
    }

    // =================================================================================
    // UI
    // =================================================================================

    /** @brief The bottom area's Timeline tab: header controls in `hb`, the dope sheet in `body`. */
    void draw_timeline_(imm::Context& ctx, const imm::Box& hb, const imm::Box& body, float x) {
        using I = imm::Icon;
        const float bh = hb.h - 6;
        timeline_hovered_ = ctx.is_hovered(body) || ctx.is_hovered(hb);
        if (!anim_rig_) {
            const ObjectId sel = doc_.primary();
            ctx.text_in({x, hb.y, 360, hb.h}, sel ? "No Animator on this object or its parents" : "Select an object to animate",
                        ctx.style.text_dim, 0.0f);
            if (sel && active_type_ == AssetType::Scene) {
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
        const std::string rig_name = get_string(*doc_.find(anim_rig_), "name", "Rig");
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

    /** @brief The dope sheet: one row per rig object (plus Summary), a frame ruler, key diamonds, the playhead. */
    void draw_dope_sheet_(imm::Context& ctx, const imm::Box& body) {
        const auto& in = ctx.input();
        const float name_w = 170.0f, ruler_h = 20.0f, row_h = 20.0f;
        const imm::Box names{body.x, body.y, name_w, body.h};
        const imm::Box track{body.x + name_w, body.y, body.w - name_w, body.h};
        ctx.fill(body, ctx.style.panel_bg);
        ctx.fill(names, ctx.style.panel_alt);
        const float len = std::max(anim_clip_.model.length, 1.0f / kAnimFps);
        const float x0 = track.x + 10, x1 = track.right() - 14;
        const float pps = (x1 - x0) / len;
        auto x_of = [&](float t) { return x0 + t * pps; };
        auto t_of = [&](float x) { return (x - x0) / pps; };

        // Ruler.
        const imm::Box ruler{track.x, track.y, track.w, ruler_h};
        ctx.fill(ruler, ctx.style.header);
        const int frames = static_cast<int>(std::round(len * kAnimFps));
        int label_every = 1;
        while (label_every * pps / kAnimFps < 34.0f) label_every *= (label_every == 1 ? 5 : 2);
        for (int f = 0; f <= frames; ++f) {
            const float x = x_of(f / kAnimFps);
            const bool major = f % label_every == 0;
            if (!major && pps / kAnimFps < 5.0f) continue;
            ctx.line({x, ruler.bottom() - (major ? 7.0f : 3.0f)}, {x, ruler.bottom()}, ctx.style.text_dim, 1.0f);
            if (major) ctx.draw_text({x + 2, ruler.y + 2}, std::to_string(f), ctx.style.text_dim, 10.0f);
            if (major) ctx.line({x, ruler.bottom()}, {x, track.bottom()}, imm::with_alpha(ctx.style.text_dim, 0.12f), 1.0f);
        }
        // The clip's end.
        ctx.line({x_of(len), track.y}, {x_of(len), track.bottom()}, imm::with_alpha(ctx.style.accent, 0.5f), 1.0f);

        // Rows: Summary, then the rig's objects.
        struct Row { std::string label; std::string path; bool summary; ObjectId id; int depth; };
        std::vector<Row> rows{{"Summary", "", true, 0, 0}};
        for (const auto& [id, depth] : rig_objects_(anim_rig_)) {
            rows.push_back({get_string(*doc_.find(id), "name"), rig_path_of_(anim_rig_, id), false, id, depth});
        }
        const glm::vec2 m = ctx.mouse();
        const bool in_track = ctx.is_hovered(track) && m.y > ruler.bottom();
        // 1 if (row, t) is among the dragged (selected) keys -- they draw at their dragged position.
        auto drag_sel = [&](const Row& row, float t) -> float {
            if (row.summary) {
                for (const auto& k : anim_sel_keys_) if (std::abs(k.time - t) <= ClipModel::kTimeEps) return 1.0f;
                return 0.0f;
            }
            return anim_sel_keys_.count({row.path, t}) ? 1.0f : 0.0f;
        };
        bool on_key = false;
        ctx.push_clip(body);
        for (size_t r = 0; r < rows.size(); ++r) {
            const float y = body.y + ruler_h + r * row_h;
            if (y > body.bottom()) break;
            const Row& row = rows[r];
            const imm::Box rb{body.x, y, body.w, row_h};
            if (r % 2) ctx.fill(rb, glm::vec4(1, 1, 1, 0.025f));
            const bool selected = !row.summary && doc_.is_selected(row.id);
            if (selected) ctx.fill({names.x, y, names.w, row_h}, imm::with_alpha(ctx.style.selection, 0.6f));
            ctx.text_in({names.x + 6 + row.depth * 12.0f, y, names.w - 8, row_h}, row.label,
                        row.summary ? ctx.style.text_dim : ctx.style.text, 0.0f);
            // Clicking a name selects the object (Shift adds).
            if (!row.summary && ctx.is_hovered({names.x, y, names.w, row_h}) && in.pressed[0]) {
                doc_.select(row.id, has(in.mods, Mods::Shift));
            }
            const std::vector<float> times = row.summary ? anim_clip_.model.key_times() : anim_clip_.model.key_times(&row.path);
            for (float t : times) {
                const glm::vec2 c{x_of(t) + (anim_drag_.active ? anim_drag_.dt * pps * drag_sel(row, t) : 0.0f), y + row_h * 0.5f};
                bool sel_key = false;
                if (row.summary) {
                    for (const auto& k : anim_sel_keys_) sel_key |= std::abs(k.time - t) <= ClipModel::kTimeEps;
                } else {
                    sel_key = anim_sel_keys_.count({row.path, t}) > 0;
                }
                const float rr = row.summary ? 5.5f : 4.5f;
                const glm::vec4 fillc = sel_key ? glm::vec4(1.0f, 0.62f, 0.18f, 1) : glm::vec4(0.82f, 0.82f, 0.82f, 1);
                ctx.triangle({c.x, c.y - rr}, {c.x + rr, c.y}, {c.x, c.y + rr}, fillc);
                ctx.triangle({c.x, c.y - rr}, {c.x, c.y + rr}, {c.x - rr, c.y}, fillc);
                if (in_track && in.pressed[0] && glm::distance(m, c) <= rr + 2.0f && !on_key) {
                    on_key = true;
                    // Select this cell (Summary: every object's keys at this time); Shift toggles.
                    std::set<ClipModel::KeyRef> cells;
                    if (row.summary) {
                        for (const auto& rw : rows) {
                            if (rw.summary) continue;
                            for (float kt : anim_clip_.model.key_times(&rw.path)) {
                                if (std::abs(kt - t) <= ClipModel::kTimeEps) cells.insert({rw.path, kt});
                            }
                        }
                    } else {
                        cells.insert({row.path, t});
                    }
                    if (has(in.mods, Mods::Shift)) {
                        for (const auto& c2 : cells) {
                            if (anim_sel_keys_.count(c2)) anim_sel_keys_.erase(c2);
                            else anim_sel_keys_.insert(c2);
                        }
                    } else if (!sel_key) {
                        anim_sel_keys_ = cells;
                    }
                    anim_drag_ = {true, m.x, 0.0f};
                }
            }
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
        } else if (in_track && in.pressed[0] && !on_key) {
            anim_sel_keys_.clear();   // empty space
        }

        // Scrubbing on the ruler (or anywhere with the right... no: ruler only, keys own the rows).
        if ((ctx.is_hovered(ruler) && in.pressed[0]) || anim_scrubbing_) {
            anim_scrubbing_ = in.down[0];
            const float t = std::clamp(t_of(m.x), 0.0f, len);
            anim_time_ = has(in.mods, Mods::Shift) ? t : anim_snap_(t);
        }
        // The playhead.
        const float px = x_of(anim_time_);
        ctx.line({px, track.y}, {px, track.bottom()}, glm::vec4(0.32f, 0.55f, 0.95f, 1), 2.0f);
        const imm::Box tag{px - 14, ruler.y + 1, 28, ruler_h - 2};
        ctx.fill_rounded(tag, glm::vec4(0.32f, 0.55f, 0.95f, 1), 3);
        ctx.text_in(tag, std::to_string(static_cast<int>(std::round(anim_time_ * kAnimFps))), glm::vec4(1), 0.0f, true);

        // Keys over the panel.
        if (timeline_hovered_ && !ctx.wants_keyboard() && !ctx.any_popup_open()) {
            if (ctx.shortcut(Key::Space)) anim_playing_ = !anim_playing_;
            if (ctx.shortcut(Key::I)) anim_insert_keys_();
            if (ctx.shortcut(Key::X) || ctx.shortcut(Key::Delete)) anim_delete_selected_keys_();
            if (ctx.shortcut(Key::Right)) anim_time_ = anim_snap_(anim_time_ + 1.0f / kAnimFps);
            if (ctx.shortcut(Key::Left)) anim_time_ = anim_snap_(anim_time_ - 1.0f / kAnimFps);
            if (ctx.shortcut(Key::Home)) anim_time_ = 0.0f;
        }
    }

public:
    // --- Animation (tests, menus) ---
    ObjectId animation_rig() const { return anim_rig_; }
    const ClipModel* animation_clip() const { return anim_clip_.open() ? &anim_clip_.model : nullptr; }
    std::string animation_clip_state() const { return anim_clip_.state; }
    float animation_time() const { return anim_time_; }
    void set_animation_time(float t) { anim_time_ = std::max(0.0f, t); }
    void set_animation_record(bool on) { anim_record_ = on; }
    bool animation_record() const { return anim_record_; }
    void show_timeline() { show_bottom_ = true; bottom_view_ = 1; }
    void set_timeline_rest_pose(bool on) { anim_show_rest_ = on; }
    void add_animator(ObjectId id) { anim_add_animator_(id); }
    void new_animation_clip(const std::string& name = "Clip") { anim_new_clip_(name); }
    void insert_keyframes() { anim_insert_keys_(); }
    /** @brief Moves an object the way the gizmo does (the live pose while recording a rig object). */
    void set_object_transform(ObjectId id, const glm::vec3& p, const glm::vec3& r, const glm::vec3& s) {
        set_object_transform_(id, p, r, s, "Move");
    }

private:
