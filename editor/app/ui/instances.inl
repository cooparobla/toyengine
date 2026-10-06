// editor/app/ui/instances.inl -- included inside EditorApp's class body.
//
// Prefab instances (`prefab: objects/crate`) and their overrides.
//
// The document holds an instance as its root node plus, for every child the instance inherits
// from its object asset, a placeholder node (SceneDocument::sync_placeholders()) carrying that
// child's overrides. Placeholders give inherited children ids, so selection, the Hierarchy,
// the gizmo and picking treat them like any object. What the editor SHOWS for a node inside an
// instance is its resolved node (asset + overrides, instance_view_()); what it WRITES is the
// smallest override that turns the asset's value into the edited one, and nothing at all when
// they agree -- the object asset itself is never touched, except by Apply to Object Asset.
//
//   Overrides are matched as SceneInheritance merges them: components by type (+ `id`),
//   children by name, mappings key by key.

    // --- resolving ----------------------------------------------------------------------

    /** @brief Documents read while resolving, reused while their files are unchanged. */
    struct CachedDoc { fs::file_time_type time{}; Node node; };
    mutable std::map<std::string, CachedDoc> inherit_docs_;

    coopa::scene::SceneInheritance::DocumentLoader inherit_loader_() const {
        return [this](const std::string& p) -> Node {
            const fs::path file = coopa::yaml::resolve_variant(p);
            std::error_code ec;
            const auto t = fs::last_write_time(file, ec);
            auto it = inherit_docs_.find(file.string());
            if (!ec && it != inherit_docs_.end() && it->second.time == t) return it->second.node;
            Node n = coopa::yaml::load_document(file);
            if (!ec) inherit_docs_[file.string()] = {t, n};
            return n;
        };
    }

    std::string inherit_anchor_() const { return (doc_.path().empty() ? sync_.fallback_path : doc_.path()).string(); }

    /** @brief `obj` with its prefab expanded, keeping editor ids (null if it can't be resolved). */
    std::optional<Node> resolve_keep_ids_(const Node& obj) const {
        try {
            return coopa::scene::SceneInheritance::resolve_object(obj, inherit_anchor_(), inherit_loader_());
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    /** @brief The object asset an instance node refers to, as the asset has it (no overrides). */
    std::optional<Node> bare_asset_of_(const Node& inst) const {
        Node bare = Node::mapping();
        bare["name"] = inst.contains("name") ? inst.at("name") : Node(std::string("x"));
        for (const char* k : {"prefab", "inherit_from"}) if (inst.contains(k)) bare[k] = inst.at(k);
        return resolve_keep_ids_(bare);
    }

    /** @brief Placeholders for every inherited child (see the file doc). Before each rebuild. */
    void sync_instance_placeholders_() {
        doc_.sync_placeholders([this](const Node& inst) { return bare_asset_of_(inst); });
    }

    /** @brief One outermost instance, resolved: its nodes merged (`resolved`) and as its asset has them (`base`). */
    struct InstanceEntry {
        Node src;   ///< The instance's document node this was resolved from.
        std::unordered_map<ObjectId, Node> resolved, base;
    };
    /** @brief Every instance in the document, resolved; re-resolved per instance only when its node changed. */
    struct InstanceView {
        uint64_t rev = ~0ull, gen = ~0ull;
        std::map<ObjectId, InstanceEntry> roots;
        std::unordered_map<ObjectId, const Node*> resolved, base;   ///< Into `roots`.
    };
    mutable InstanceView inst_view_;
    uint64_t scene_gen_ = 0;   ///< Bumped by every rebuild: asset files may have changed since.

    InstanceEntry resolve_instance_entry_(const Node& inst) const {
        InstanceEntry e;
        e.src = inst;
        std::function<void(const Node&)> collect = [&](const Node& n) {
            if (const ObjectId id = SceneDocument::id_of(n)) e.resolved[id] = n;
            if (n.contains("children") && n.at("children").is_sequence()) for (const auto& c : n.at("children").as_seq()) collect(c);
        };
        // base: the doc's instance subtree walked alongside its bare asset, by child name.
        std::function<void(const Node&, const Node&)> pair_base = [&](const Node& doc_node, const Node& asset) {
            e.base[SceneDocument::id_of(doc_node)] = asset;
            if (!doc_node.contains("children") || !doc_node.at("children").is_sequence()) return;
            for (const auto& c : doc_node.at("children").as_seq()) {
                if (SceneDocument::is_instance_node(c)) {
                    if (auto b = bare_asset_of_(c)) pair_base(c, *b);
                    continue;
                }
                if (!c.contains(kInheritedKey) || !asset.contains("children")) continue;
                const std::string n = get_string(c, "name");
                for (const auto& ac : asset.at("children").as_seq()) {
                    if (get_string(ac, "name") == n) { pair_base(c, ac); break; }
                }
            }
        };
        if (auto r = resolve_keep_ids_(inst)) collect(*r);   // instances nested in it included
        if (auto b = bare_asset_of_(inst)) pair_base(inst, *b);
        return e;
    }

    const InstanceView& instance_view_() const {
        if (inst_view_.rev == doc_.undo_revision() && inst_view_.gen == scene_gen_) return inst_view_;
        // A slider drag on one instance changes the document every frame: only that instance's
        // node differs, so every other instance keeps its resolution (no asset reads).
        const bool same_files = inst_view_.gen == scene_gen_;
        InstanceView v;
        v.rev = doc_.undo_revision();
        v.gen = scene_gen_;
        std::function<void(const Node&)> walk = [&](const Node& list) {
            if (!list.is_sequence()) return;
            for (const auto& o : list.as_seq()) {
                if (SceneDocument::is_instance_node(o)) {
                    const ObjectId id = SceneDocument::id_of(o);
                    auto old = inst_view_.roots.find(id);
                    if (same_files && old != inst_view_.roots.end() && old->second.src == o) v.roots[id] = std::move(old->second);
                    else v.roots[id] = resolve_instance_entry_(o);
                    continue;
                }
                if (o.contains("children")) walk(o.at("children"));
            }
        };
        walk(doc_.root_objects());
        for (const auto& [rid, e] : v.roots) {
            for (const auto& [id, n] : e.resolved) v.resolved[id] = &n;
            for (const auto& [id, n] : e.base) v.base[id] = &n;
        }
        inst_view_ = std::move(v);
        return inst_view_;
    }

    /** @brief The merged node of an object inside an instance, or null for a plain object. */
    const Node* resolved_of_(ObjectId id) const {
        const auto& v = instance_view_();
        auto it = v.resolved.find(id);
        return it == v.resolved.end() ? nullptr : it->second;
    }
    /** @brief The object asset's own node for an object inside an instance, or null. */
    const Node* base_of_(ObjectId id) const {
        const auto& v = instance_view_();
        auto it = v.base.find(id);
        return it == v.base.end() ? nullptr : it->second;
    }
    /** @brief What the editor shows for `obj`: its merged node inside an instance, else itself. */
    const Node& effective_(const Node& obj) const {
        // A map lookup, not a walk up the document: this runs for every object, every frame.
        // Only objects inside an instance are in the resolved view.
        if (const ObjectId id = SceneDocument::id_of(obj)) if (const Node* r = resolved_of_(id)) return *r;
        return obj;
    }
    /** @brief True for an instance root or a child it inherits: edits there are overrides. */
    bool in_instance_(ObjectId id) const { return doc_.instance_root_of(id) != 0; }

    // --- reading and writing overrides ----------------------------------------------------

    /** @brief Index in `list` of the component matching `comp` by type and `id`, or -1. */
    static int match_component_(const Node& list, const Node& comp) {
        if (!list.is_sequence()) return -1;
        const std::string t = component_type(comp);
        const std::string cid = comp.contains("id") ? comp.at("id").get_value<std::string>() : "";
        const auto& seq = list.as_seq();
        for (size_t k = 0; k < seq.size(); ++k) {
            if (component_type(seq[k]) != t) continue;
            const std::string oid = seq[k].contains("id") ? seq[k].at("id").get_value<std::string>() : "";
            if (oid == cid) return static_cast<int>(k);
        }
        return -1;
    }

    /** @brief `id`'s own override entry for `comp` (null if it overrides nothing there). */
    const Node* own_override_(ObjectId id, const Node& comp) const {
        const Node* o = doc_.find(id);
        if (!o || !o->contains("components")) return nullptr;
        const int k = match_component_(o->at("components"), comp);
        return k < 0 ? nullptr : &o->at("components").as_seq()[static_cast<size_t>(k)];
    }

    /** @brief The asset's component matching `comp` for an object inside an instance (null if added). */
    const Node* base_component_(ObjectId id, const Node& comp) const {
        const Node* b = base_of_(id);
        if (!b || !b->contains("components")) return nullptr;
        const int k = match_component_(b->at("components"), comp);
        return k < 0 ? nullptr : &b->at("components").as_seq()[static_cast<size_t>(k)];
    }

    /** @brief A mapping whose values are all scalars (a vector, a colour): compared whole. */
    static bool leafy_(const Node& n) {
        if (!n.is_mapping()) return true;
        for (const auto& kv : n.as_map()) if (kv.second.is_mapping() || kv.second.is_sequence()) return false;
        return true;
    }

    /** @brief The keys of `edited` that differ from `base` (mappings recurse, key by key). */
    static Node diff_map_(const Node& base, const Node& edited) {
        Node out = Node::mapping();
        for (const auto& [k, v] : edited.as_map()) {
            const std::string key = k.get_value<std::string>();
            if (key.rfind("__", 0) == 0) continue;
            if (!base.contains(key)) { out[key] = v; continue; }
            const Node& b = base.at(key);
            if (b == v) continue;
            if (b.is_mapping() && v.is_mapping() && !leafy_(v) && !leafy_(b)) {
                Node d = diff_map_(b, v);
                if (d.size() > 0) out[key] = d;
            } else {
                out[key] = v;
            }
        }
        return out;
    }

    /** @brief The smallest override turning the asset's component into `edited` (type + id kept). */
    Node minimal_override_(ObjectId id, const Node& edited) const {
        Node ov = Node::mapping();
        ov["type"] = Node(component_type(edited));
        if (edited.contains("id")) ov["id"] = edited.at("id");
        const Node* base = base_component_(id, edited);
        const Node d = base ? diff_map_(*base, edited) : diff_map_(Node::mapping(), edited);
        for (const auto& [k, v] : d.as_map()) {
            const std::string key = k.get_value<std::string>();
            if (key != "type" && key != "id") ov[key] = v;
        }
        return ov;
    }

    /** @brief True when an override entry holds nothing but its type / id. */
    static bool override_empty_(const Node& ov) { return ov.size() <= (ov.contains("id") ? 2u : 1u); }

    /** @brief Writes (or, when it overrides nothing, removes) `id`'s override entry for `match`. */
    void write_override_(ObjectId id, const Node& match, const Node& ov, const std::string& merge = {}) {
        apply_(doc_.edit("Override " + component_type(match), [&](Node&) -> Change {
            Node* o = doc_.find(id);
            if (!o) return {};
            if (!o->contains("components")) (*o)["components"] = Node::sequence();
            auto& seq = (*o)["components"].as_seq();
            const int k = match_component_((*o)["components"], match);
            if (override_empty_(ov)) { if (k >= 0) seq.erase(seq.begin() + k); else return {}; }
            else if (k >= 0) seq[static_cast<size_t>(k)] = ov;
            else seq.push_back(ov);
            return {ChangeScope::Object, id};   // apply_() rebuilds just this instance
        }, merge));
    }

    /** @brief Drops one key of `id`'s override for `comp` (the component's whole entry if empty after). */
    void revert_override_field_(ObjectId id, const Node& comp, const std::string& key) {
        const Node* own = own_override_(id, comp);
        if (!own || !own->contains(key)) return;
        Node ov = *own;
        erase_key(ov, key);
        write_override_(id, comp, ov);
    }

    /** @brief The resolved Transform of an object inside an instance (false if it has none). */
    bool instance_transform_(ObjectId id, glm::vec3& p, glm::vec3& r, glm::vec3& s) const {
        const Node* res = resolved_of_(id);
        p = glm::vec3(0); r = glm::vec3(0); s = glm::vec3(1);
        if (!res || !res->contains("components")) return false;
        for (const auto& c : res->at("components").as_seq()) {
            if (component_type(c) != "Transform") continue;
            p = get_vec3(c, "position");
            r = get_vec3(c, "rotation");
            s = get_vec3(c, "scale", glm::vec3(1.0f));
            return true;
        }
        return false;
    }

    /**
     * @brief Moves an inherited child: its Transform override keeps only the parts that differ
     *        from the asset's, and the live object is set directly (an override key that goes
     *        away would otherwise leave the live value where it was).
     */
    void set_inherited_transform_(ObjectId id, const glm::vec3& p, const glm::vec3& r, const glm::vec3& s,
                                  const std::string& label, const std::string& merge_key) {
        Node match = Node::mapping();
        match["type"] = Node(std::string("Transform"));
        const Node* base = base_component_(id, match);
        auto differs = [&](const char* key, const glm::vec3& v, const glm::vec3& def) {
            const glm::vec3 b = base ? get_vec3(*base, key, def) : def;
            return glm::distance(b, v) > 1e-5f;
        };
        Node ov = match;
        if (differs("position", p, glm::vec3(0.0f))) ov["position"] = make_vec3(p);
        if (differs("rotation", r, glm::vec3(0.0f))) ov["rotation"] = make_vec3(r);
        if (differs("scale", s, glm::vec3(1.0f))) ov["scale"] = make_vec3(s);
        doc_.edit(label, [&](Node&) -> Change {
            Node* o = doc_.find(id);
            if (!o) return {};
            if (!o->contains("components")) (*o)["components"] = Node::sequence();
            auto& seq = (*o)["components"].as_seq();
            const int k = match_component_((*o)["components"], match);
            if (override_empty_(ov)) { if (k < 0) return {}; seq.erase(seq.begin() + k); }
            else if (k >= 0) seq[static_cast<size_t>(k)] = ov;
            else seq.insert(seq.begin(), ov);
            return {ChangeScope::Transform, id};
        }, merge_key);
        if (auto* live = sync_.live(id); live && live->get_transform()) {
            auto& t = live->get_transform()->transform();
            t.set_position(p);
            t.set_rotation(r);
            t.set_scale(s);
        }
    }

    /** @brief True when `id` (inside an instance) carries any override of its own. */
    bool has_overrides_(ObjectId id) const {
        const Node* o = doc_.find(id);
        if (!o) return false;
        if (doc_.is_instance(id)) {
            // The root's own Transform is its placement, not an override.
            if (o->contains("components")) for (const auto& c : o->at("components").as_seq()) if (component_type(c) != "Transform") return true;
            return false;
        }
        Node shallow = *o;
        erase_key(shallow, "children");
        return !is_empty_placeholder(shallow);
    }

    /** @brief Removes every override `id` (an instance root or inherited child) holds -- and below it. */
    void revert_all_overrides_(ObjectId id) {
        if (!in_instance_(id)) return;
        apply_(doc_.edit("Revert Overrides", [&](Node&) -> Change {
            Node* o = doc_.find(id);
            if (!o) return {};
            std::function<void(Node&, bool)> clear = [&](Node& n, bool root) {
                if (n.contains("components") && n.at("components").is_sequence()) {
                    auto& seq = n["components"].as_seq();
                    // An instance root keeps its placement.
                    seq.erase(std::remove_if(seq.begin(), seq.end(), [&](const Node& c) { return !(root && component_type(c) == "Transform"); }), seq.end());
                }
                if (!root) for (const auto& k : keys_of(n)) if (k != "name" && k != "components" && k != "children" && k.rfind("__", 0) != 0) erase_key(n, k);
                if (n.contains("children") && n.at("children").is_sequence()) {
                    auto& kids = n["children"].as_seq();
                    // Children the instance added go too: the instance becomes the asset again.
                    kids.erase(std::remove_if(kids.begin(), kids.end(), [](const Node& c) { return !c.contains(kInheritedKey); }), kids.end());
                    for (auto& c : kids) clear(c, false);
                }
            };
            clear(*o, doc_.is_instance(id));
            return {ChangeScope::Object, id};
        }));
        doc_.prune_selection();   // the children it added are gone
    }

    /** @brief `id`'s first `type` component as the editor shows it (resolved inside an instance). */
    bool shown_component_(ObjectId id, const std::string& type, Node& out) const {
        const Node* o = doc_.find(id);
        if (!o) return false;
        const Node& e = effective_(*o);
        if (!e.contains("components") || !e.at("components").is_sequence()) return false;
        for (const auto& c : e.at("components").as_seq()) if (component_type(c) == type) { out = c; return true; }
        return false;
    }

    /** @brief Saves an edited component the editor showed: in place, or inside an instance as its smallest override. */
    void edit_component_(ObjectId id, const Node& edited, const std::string& label, const std::string& merge = {}) {
        if (in_instance_(id)) { write_override_(id, edited, minimal_override_(id, edited), merge); return; }
        const Node* o = doc_.find(id);
        const int k = o && o->contains("components") ? match_component_(o->at("components"), edited) : -1;
        if (k >= 0) apply_(doc_.set_component(id, k, edited, label, merge));
    }

    // --- Apply to Object Asset --------------------------------------------------------------

    /** @brief The object asset file instance root `root` refers to (empty if it can't be found). */
    fs::path instance_asset_file_(ObjectId root) const {
        const Node* o = doc_.find(root);
        if (!o) return {};
        const std::string ref = get_string(*o, "prefab", get_string(*o, "inherit_from"));
        if (ref.empty()) return {};
        const std::string item = ref + (fs::path(ref).has_extension() ? "" : ".yaml");
        fs::path abs = project_.absolute(item);
        if (!coopa::yaml::document_exists(abs) && !doc_.path().empty()) abs = doc_.path().parent_path() / item;
        return coopa::yaml::document_exists(abs) ? abs : fs::path{};
    }

    /** @brief The asset path of instance root `root` as the Asset panel names it ("objects/crate.yaml"). */
    std::string instance_asset_item_(ObjectId root) const {
        const Node* o = doc_.find(root);
        const std::string ref = o ? get_string(*o, "prefab", get_string(*o, "inherit_from")) : std::string();
        return ref.empty() ? ref : ref + (fs::path(ref).has_extension() ? "" : ".yaml");
    }

    /** @brief SceneInheritance's merge of one override onto a base value: mappings key by key. */
    static void merge_into_(Node& base, const Node& over) {
        for (const auto& [k, v] : over.as_map()) {
            const std::string key = k.get_value<std::string>();
            if (base.contains(key) && base.at(key).is_mapping() && v.is_mapping()) merge_into_(base[key], v);
            else base[key] = v;
        }
    }

    /**
     * @brief Writes overrides into the object asset itself: `comps` (override entries) and
     *        `extra` (other keys, e.g. `active`) onto the asset's object at `path` (child names
     *        from its root), and `added` children appended there. A path into an instance the
     *        asset itself contains becomes an override node in the asset -- the same merge.
     * @return False (logged) if the asset can't be written.
     */
    bool write_to_asset_(ObjectId root, const std::vector<std::string>& path, const std::vector<Node>& comps,
                         const Node& extra, const std::vector<Node>& added) {
        const fs::path file = instance_asset_file_(root);
        if (file.empty()) { log_error("Object asset not found"); return false; }
        if (refuse_engine_asset_(file)) return false;
        try {
            Node doc = coopa::yaml::load_document(coopa::yaml::resolve_variant(file));
            if (!doc.is_mapping() || !doc.contains("object")) { log_error("Not an object asset: " + file.string()); return false; }
            Node* cur = &doc["object"];
            for (const auto& n : path) {
                Node& kids = ensure_seq(*cur, "children");
                Node* found = nullptr;
                for (auto& c : kids.as_seq()) if (get_string(c, "name") == n) { found = &c; break; }
                if (!found) {
                    Node c = Node::mapping();
                    c["name"] = Node(n);
                    kids.as_seq().push_back(c);
                    found = &kids.as_seq().back();
                }
                cur = found;
            }
            Node& list = ensure_seq(*cur, "components");
            for (Node ov : comps) {
                strip_private_keys(ov);
                const int k = match_component_(list, ov);
                if (get_bool(ov, "remove", false)) { if (k >= 0) list.as_seq().erase(list.as_seq().begin() + k); continue; }
                if (k >= 0) merge_into_(list.as_seq()[static_cast<size_t>(k)], ov);
                else list.as_seq().push_back(ov);
            }
            if (extra.is_mapping()) merge_into_(*cur, extra);
            for (Node c : added) {
                strip_private_keys(c);
                ensure_seq(*cur, "children").as_seq().push_back(c);
            }
            coopa::yaml::save_document(file, doc);
            inherit_docs_.clear();
            project_.refresh();
            log_info("Applied to " + project_.relative(file));
            return true;
        } catch (const std::exception& e) {
            log_error(std::string("Apply to Object Asset failed: ") + e.what());
            return false;
        }
    }

    /** @brief Apply one field (`key`) of `id`'s override for `comp` to the asset, then drop it here. */
    void apply_field_to_asset_(ObjectId id, const Node& comp, const std::string& key) {
        const Node* own = own_override_(id, comp);
        const ObjectId root = doc_.instance_root_of(id);
        if (!own || !own->contains(key) || !root) return;
        Node part = Node::mapping();
        part["type"] = Node(component_type(comp));
        if (comp.contains("id")) part["id"] = comp.at("id");
        part[key] = own->at(key);
        if (!write_to_asset_(root, doc_.path_in_instance(root, id), {part}, Node::mapping(), {})) return;
        revert_override_field_(id, comp, key);
        queue_rebuild_();
    }

    /** @brief Apply `id`'s whole override for `comp` to the asset, then drop it here. */
    void apply_component_to_asset_(ObjectId id, const Node& comp) {
        const Node* own = own_override_(id, comp);
        const ObjectId root = doc_.instance_root_of(id);
        if (!own || !root) return;
        if (!write_to_asset_(root, doc_.path_in_instance(root, id), {*own}, Node::mapping(), {})) return;
        Node empty = Node::mapping();
        empty["type"] = Node(component_type(comp));
        if (comp.contains("id")) empty["id"] = comp.at("id");
        write_override_(id, comp, empty);
        queue_rebuild_();
    }

    /** @brief Apply every override of `id` and below (an instance root: the whole instance) to the asset. */
    void apply_all_to_asset_(ObjectId id) {
        const ObjectId root = doc_.instance_root_of(id);
        const Node* start = doc_.find(id);
        if (!root || !start) return;
        // Gather first (the asset is written once per object), then clear the instance.
        bool ok = true;
        std::function<void(const Node&, ObjectId)> gather = [&](const Node& n, ObjectId nid) {
            const bool is_root = nid == root;
            std::vector<Node> comps;
            if (n.contains("components")) for (const auto& c : n.at("components").as_seq()) if (!(is_root && component_type(c) == "Transform")) comps.push_back(c);
            Node extra = Node::mapping();
            if (!is_root) for (const auto& k : keys_of(n)) if (k != "name" && k != "components" && k != "children" && k.rfind("__", 0) != 0) extra[k] = n.at(k);
            std::vector<Node> added;
            if (n.contains("children")) for (const auto& c : n.at("children").as_seq()) if (!c.contains(kInheritedKey)) added.push_back(c);
            if (!comps.empty() || extra.size() > 0 || !added.empty()) ok &= write_to_asset_(root, doc_.path_in_instance(root, nid), comps, extra, added);
            if (n.contains("children")) {
                for (const auto& c : n.at("children").as_seq()) if (c.contains(kInheritedKey)) gather(c, SceneDocument::id_of(c));
            }
        };
        gather(*start, id);
        if (ok) revert_all_overrides_(id);
        queue_rebuild_();
    }

    // --- selection ---------------------------------------------------------------------------

    /**
     * @brief What a click on `hit` selects. A part of an instance selects the whole instance
     *        first; once the instance (or something in it) is selected, the part itself --
     *        Unity's prefab selection.
     */
    ObjectId click_target_(ObjectId hit) const {
        const ObjectId root = doc_.outermost_instance_of(hit);
        if (!root || root == hit) return hit;
        for (ObjectId s : doc_.selection()) if (doc_.outermost_instance_of(s) == root) return hit;
        return root;
    }

    /** @brief Box select picks whole instances, not their parts. */
    ObjectId box_target_(ObjectId id) const {
        const ObjectId root = doc_.outermost_instance_of(id);
        return root ? root : id;
    }

    /** @brief Instance commands for a context menu on `id` (shown only for an object in an instance). */
    void draw_instance_menu_items_(imm::Context& ctx, ObjectId id) {
        using I = imm::Icon;
        const ObjectId root = id ? doc_.instance_root_of(id) : 0;
        if (!root) return;
        const bool ok = !playing();
        if (ctx.menu_item("Open Object Asset", "", nullptr, true, I::Object)) open_asset(AssetType::Object, instance_asset_item_(root));
        ctx.tooltip("Open Object Asset\nEdit " + instance_asset_item_(root) + " itself (every instance follows)");
        if (ctx.menu_item("Select Instance Root", "", nullptr, root != id, I::Link)) doc_.select(root);
        const bool any = has_overrides_(id) || [&] {
            bool a = false;
            if (const Node* n = doc_.find(id)) {
                std::function<void(const Node&)> walk = [&](const Node& x) {
                    if (!x.contains("children")) return;
                    for (const auto& c : x.at("children").as_seq()) { a |= !c.contains(kInheritedKey) || has_overrides_(SceneDocument::id_of(c)); walk(c); }
                };
                walk(*n);
            }
            return a;
        }();
        if (ctx.menu_item(root == id ? "Revert Instance Overrides" : "Revert Overrides", "", nullptr, ok && any, I::Restart)) revert_all_overrides_(id);
        ctx.tooltip("Revert\nDrop the overrides (and added children) here, back to the object asset's values");
        if (ctx.menu_item(root == id ? "Apply Instance Overrides to Asset" : "Apply Overrides to Asset", "", nullptr, ok && any, I::Save)) apply_all_to_asset_(id);
        ctx.tooltip("Apply to Object Asset\nWrite these overrides into the object asset, changing every instance");
        ctx.menu_separator();
    }

    // --- renaming a part of an object asset ---------------------------------------------------

    /// The open object asset's objects (by id) and their name paths, as of opening / saving it.
    std::map<ObjectId, std::vector<std::string>> asset_name_paths_;

    std::map<ObjectId, std::vector<std::string>> asset_name_paths_now_() const {
        std::map<ObjectId, std::vector<std::string>> out;
        const ObjectId root = doc_.object_root();
        if (!root) return out;
        for (ObjectId id : doc_.all_ids()) {
            if (id == root || doc_.is_inherited(id)) continue;
            out[id] = doc_.path_in_instance(root, id);
        }
        return out;
    }
    void remember_asset_names_() { asset_name_paths_ = asset_name_paths_now_(); }

    /**
     * @brief After saving object asset `asset_file`: overrides find its children by name, so a
     *        child renamed here is renamed in every scene and object asset that overrides it.
     * @return The files rewritten.
     */
    std::vector<fs::path> follow_asset_renames_(const fs::path& asset_file) {
        struct Rename { std::vector<std::string> parent; std::string from, to; };
        std::vector<Rename> renames;
        for (const auto& [id, now] : asset_name_paths_now_()) {
            auto it = asset_name_paths_.find(id);
            if (it == asset_name_paths_.end() || it->second.size() != now.size() || now.empty() || it->second.back() == now.back()) continue;
            renames.push_back({std::vector<std::string>(now.begin(), now.end() - 1), it->second.back(), now.back()});
        }
        remember_asset_names_();
        std::vector<fs::path> rewritten;
        if (renames.empty()) return rewritten;
        // Shallow first: a deeper rename's parent path already uses the new names.
        std::sort(renames.begin(), renames.end(), [](const Rename& a, const Rename& b) { return a.parent.size() < b.parent.size(); });
        const std::string ref = strip_yaml_ext(project_.relative(asset_file));
        auto rename_in = [&](Node& inst) {
            bool changed = false;
            for (const auto& r : renames) {
                Node* cur = &inst;
                for (const auto& n : r.parent) {
                    Node* next = nullptr;
                    if (cur->contains("children") && cur->at("children").is_sequence()) {
                        for (auto& c : (*cur)["children"].as_seq()) if (get_string(c, "name") == n) { next = &c; break; }
                    }
                    cur = next;
                    if (!cur) break;
                }
                if (!cur || !cur->contains("children")) continue;
                for (auto& c : (*cur)["children"].as_seq()) {
                    if (get_string(c, "name") == r.from) { c["name"] = Node(r.to); changed = true; break; }
                }
            }
            return changed;
        };
        std::function<bool(Node&)> walk = [&](Node& n) {
            bool changed = false;
            if (n.is_mapping()) {
                for (const char* k : {"prefab", "inherit_from"}) {
                    if (n.contains(k) && n.at(k).is_string() && strip_yaml_ext(n.at(k).get_value<std::string>()) == ref) changed |= rename_in(n);
                }
                for (auto& kv : n.as_map()) changed |= walk(kv.second);
            } else if (n.is_sequence()) {
                for (auto& e : n.as_seq()) changed |= walk(e);
            }
            return changed;
        };
        std::error_code ec;
        for (auto e = fs::recursive_directory_iterator(project_.assets(), ec); e != fs::recursive_directory_iterator(); e.increment(ec)) {
            if (ec) break;
            if (!e->is_regular_file() || e->path().extension() != ".yaml" || fs::equivalent(e->path(), asset_file, ec)) continue;
            try {
                Node doc = coopa::yaml::load_document(e->path());
                if (!walk(doc)) continue;
                coopa::yaml::save_document(e->path(), doc);
                rewritten.push_back(e->path());
            } catch (const std::exception&) {
                continue;   // not YAML this can read (or write): nothing of ours in it
            }
        }
        for (const auto& f : rewritten) log_info("Renamed overrides in " + project_.relative(f));
        return rewritten;
    }
