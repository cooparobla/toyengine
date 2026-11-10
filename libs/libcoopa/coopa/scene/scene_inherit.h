/**
 * @file scene_inherit.h
 * @brief Resolves `inherit_from` references in a scene document before parsing.
 *
 * This is a pure YAML-node preprocessing pass: it never touches Scene,
 * SceneObject, or Component. SceneLoader::load() runs it once on the freshly
 * loaded root node, and everything downstream (parse_object_, the component
 * parser registry) is unaware inheritance exists at all.
 *
 * Two independent inheritance points are supported:
 *   - Object-level: any object node (in root_objects or children) may carry
 *     `inherit_from`, naming another file (or list of files) whose object is
 *     merged in as a base before this node's own fields apply.
 *   - Scene-level: the top-level `scene:` block may carry `inherit_from`,
 *     naming another whole scene file whose `scene:` block (name,
 *     auto_transform, root_objects) is merged in as a base.
 *
 * A base reference is `path/to/file.yaml` or `path/to/file.yaml#ObjectName`.
 * The referenced document is resolved with these rules, in order:
 *   - a top-level `object:` key names the base object directly (the
 *     canonical shape for a reusable prefab file);
 *   - a top-level `scene:` block uses `#ObjectName` to pick one of its
 *     `root_objects` (searched depth-first), or its first root object if no
 *     anchor is given;
 *   - otherwise the document's bare top-level mapping is used as-is.
 *
 * Merge rules:
 *   - Plain keys: the override's value wins; two mapping values merge
 *     recursively (so `color: { a: 0.5 }` overrides only alpha); scalars and
 *     sequences (other than components/children) replace wholesale.
 *   - `components:` entries match by normalized type/tag name, with an
 *     optional `id:` key to disambiguate repeated types (e.g. four
 *     ColorOnSignal components on one object). Matched entries deep-merge in
 *     place; unmatched entries append; an ambiguous match (repeated type, no
 *     `id`) appends instead of guessing, with a warning.
 *   - `children:` entries match by `name`. Matched children merge in place
 *     (preserving the base's position — draw/traversal order is significant
 *     throughout this codebase); unmatched children append.
 *   - An `Animator` component's `states:` entries match by `name` — the one
 *     list field inside a component that merges entry-wise rather than
 *     replacing. Matched states deep-merge in place (so a variant rig
 *     re-points one `clip` without restating the set), unmatched append.
 *     Every other sequence inside a component still replaces wholesale.
 *   - A component or child entry carrying `remove: true` deletes every
 *     matching base entry instead of merging.
 *
 * Object assets (prefabs): `prefab: objects/crate` is the same reference as `inherit_from`,
 * written for reusable object files (`objects/<name>.yaml`, shape `object: {...}`). Two things
 * differ: the extension may be omitted, and the instance's own root Transform (or, for a UI
 * prefab, RectTransform) REPLACES the prefab's (an instance is placed absolutely -- a prefab's
 * root offset must not shift every copy), while children and other components merge as above. References resolve against
 * the declaring file's folder, the root scene's folder, then the project's asset roots
 * (set_search_roots), then by name under those roots (coopa::asset::AssetIndex, so
 * `objects/crate` also finds `objects/<tags>/crate.yaml`); `objects/crate` works from any scene.
 *
 * Reserved keys, stripped or left behind as internal bookkeeping: `id` and
 * `remove` are only ever read by the merge pass; `__source_dirs` is a
 * sequence of directories (nearest first) that a node's relative asset paths
 * (`font:`, `animation_file:`, ...) should resolve against — see
 * SceneLoader::ParseContext::resolve(). Component/parser code should never
 * need to read these directly.
 */

#ifndef COOPA_SCENE_SCENE_INHERIT_H
#define COOPA_SCENE_SCENE_INHERIT_H

#include <fkYAML/node.hpp>
#include <coopa/debug/logger.h>
#include <coopa/scene/component.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <coopa/yaml/document.h>
#include <coopa/asset/asset_index.h>

namespace coopa {
namespace scene {

/**
 * @class SceneInheritance
 * @brief Static resolver that expands every `inherit_from` in a scene
 *        document into one fully-merged fkyaml::node.
 */
class SceneInheritance {
public:
    /** @brief Signature matching SceneLoader's DocumentLoader: path -> parsed root node. */
    using DocumentLoader = std::function<fkyaml::node(const std::string&)>;

    /** @brief Reserved key holding the provenance directories for asset-path resolution. */
    static constexpr const char* kSourceDirsKey = "__source_dirs";

    /**
     * @brief Resolves every `inherit_from` reachable from `path`'s document.
     *
     * @param path          Path to the root scene file (as passed to SceneLoader::load).
     * @param load_document Loader used for `path` itself and every base it references.
     * @return The fully merged root node, with no `inherit_from` keys remaining.
     * @throws std::runtime_error on a missing base file, an inheritance cycle,
     *         or a chain deeper than kMaxDepth.
     */
    static fkyaml::node resolve(const std::string& path, const DocumentLoader& load_document) {
        std::vector<std::string> stack;
        std::string root_dir = parent_dir_(path);
        return resolve_document_(path, load_document, stack, root_dir);
    }

    /**
     * @brief Expands one object node's `prefab` / `inherit_from` (and its children's) as if it
     *        were declared in `declaring_path` -- an instance spawned at runtime, or the editor
     *        showing an instance's resolved components.
     */
    static fkyaml::node resolve_object(fkyaml::node obj, const std::string& declaring_path, const DocumentLoader& load_document) {
        std::vector<std::string> stack;
        const std::string dir = std::filesystem::is_directory(declaring_path) ? declaring_path : parent_dir_(declaring_path);
        stamp_source_dir_(obj, dir);
        resolve_object_inherit_(obj, load_document, stack, dir, dir);
        return obj;
    }

    /** @brief Asset roots searched for references after the declaring / root scene folders. */
    static void set_search_roots(std::vector<std::string> roots) { search_roots_() = std::move(roots); }
    static const std::vector<std::string>& search_roots() { return search_roots_(); }

private:
    static constexpr int kMaxDepth = 32;

    static std::vector<std::string>& search_roots_() {
        static std::vector<std::string> roots;
        return roots;
    }

    // ---- small path helpers ----

    static std::string parent_dir_(const std::string& path) {
        std::string dir = std::filesystem::path(path).parent_path().string();
        return dir.empty() ? "." : dir;
    }

    static std::string canonicalize_(const std::string& path) {
        std::error_code ec;
        auto canon = std::filesystem::weakly_canonical(path, ec);
        return ec ? path : canon.string();
    }

    /** @brief Splits "path.yaml#ObjectName" into {"path.yaml", "ObjectName"} (anchor may be empty). */
    static std::pair<std::string, std::string> split_anchor_(const std::string& ref) {
        auto pos = ref.find('#');
        if (pos == std::string::npos) return {ref, ""};
        return {ref.substr(0, pos), ref.substr(pos + 1)};
    }

    /**
     * @brief Resolves a relative inherit_from path against, in order, the
     *        declaring file's directory, then the root scene's directory.
     *        Falls through unchanged so a custom document loader (e.g. a
     *        .caml container) can still make sense of it.
     */
    static std::string resolve_inherit_path_(
        const std::string& raw, const std::string& declaring_dir, const std::string& root_dir)
    {
        // An extensionless reference (`objects/crate`) means the .yaml (or its .caml twin).
        const std::string raw_path = std::filesystem::path(raw).has_extension() ? raw : raw + ".yaml";
        std::filesystem::path p(raw_path);
        if (p.is_absolute()) return raw_path;

        std::filesystem::path candidate = coopa::yaml::resolve_variant(std::filesystem::path(declaring_dir) / raw_path);
        if (std::filesystem::exists(candidate)) return candidate.string();

        candidate = coopa::yaml::resolve_variant(std::filesystem::path(root_dir) / raw_path);
        if (std::filesystem::exists(candidate)) return candidate.string();

        for (const auto& root : search_roots_()) {
            candidate = coopa::yaml::resolve_variant(std::filesystem::path(root) / raw_path);
            if (std::filesystem::exists(candidate)) return candidate.string();
        }
        // By name: `objects/crate` finds objects/<tags>/crate.yaml (AssetIndex).
        if (auto found = coopa::asset::AssetIndex::find_in(search_roots_(), raw_path)) return found->string();
        return raw_path;
    }

    // ---- tag / list helpers ----

    /** @brief The normalized type/tag name of a component node, or "" if it has neither. */
    static std::string component_tag_(const fkyaml::node& node) {
        if (node.has_tag_name()) return normalize_component_tag(node.get_tag_name());
        if (node.is_mapping() && node.contains("type")) {
            return normalize_component_tag(node.at("type").get_value<std::string>());
        }
        return "";
    }

    static bool truthy_(const fkyaml::node& node, const char* key) {
        return node.is_mapping() && node.contains(key) && node.at(key).get_value<bool>();
    }

    /** @brief Accepts either a scalar string or a sequence of strings. */
    static std::vector<std::string> as_string_list_(const fkyaml::node& node) {
        std::vector<std::string> result;
        if (node.is_sequence()) {
            for (const auto& n : node) result.push_back(n.get_value<std::string>());
        } else {
            result.push_back(node.get_value<std::string>());
        }
        return result;
    }

    static void erase_key_(fkyaml::node& node, const std::string& key) {
        if (!node.is_mapping()) return;
        auto& map = node.as_map();
        auto it = map.find(key);
        if (it != map.end()) map.erase(it);
    }

    // ---- provenance stamping ----

    /**
     * @brief Prepends `dir` to __source_dirs on this object node and every
     *        component/child beneath it, recursively.
     */
    static void stamp_source_dir_(fkyaml::node& node, const std::string& dir) {
        if (!node.is_mapping()) return;

        fkyaml::node& dirs = node[kSourceDirsKey];
        if (!dirs.is_sequence()) dirs = fkyaml::node::sequence();
        dirs.as_seq().insert(dirs.as_seq().begin(), fkyaml::node(dir));

        if (node.contains("components") && node.at("components").is_sequence()) {
            for (auto& comp : node.at("components").as_seq()) {
                if (comp.is_mapping()) {
                    fkyaml::node& comp_dirs = comp[kSourceDirsKey];
                    if (!comp_dirs.is_sequence()) comp_dirs = fkyaml::node::sequence();
                    comp_dirs.as_seq().insert(comp_dirs.as_seq().begin(), fkyaml::node(dir));
                }
            }
        }
        if (node.contains("children") && node.at("children").is_sequence()) {
            for (auto& child : node.at("children").as_seq()) stamp_source_dir_(child, dir);
        }
    }

    // ---- generic deep merge ----

    /**
     * @brief Deep-merges `over` onto `base`: shared mapping keys recurse,
     *        everything else in `over` replaces the value from `base`, and
     *        keys unique to `base` survive untouched.
     */
    static fkyaml::node merge_map_(const fkyaml::node& base, const fkyaml::node& over) {
        if (!base.is_mapping() || !over.is_mapping()) return over;

        fkyaml::node result = base;
        for (const auto& pair : over.as_map()) {
            const fkyaml::node& key = pair.first;
            const fkyaml::node& over_val = pair.second;

            if (key.is_string() && key.get_value<std::string>() == kSourceDirsKey &&
                result.contains(key) && result.at(key).is_sequence() && over_val.is_sequence()) {
                // __source_dirs is provenance bookkeeping, not scene data: keep
                // both chains reachable (override's own directory first, then
                // the base's) instead of letting the generic mapping-override
                // rule below wipe out the base's directory.
                fkyaml::node merged_dirs = over_val;
                for (const auto& d : result.at(key)) merged_dirs.as_seq().push_back(d);
                result[key] = merged_dirs;
                continue;
            }

            if (result.contains(key) && result.at(key).is_mapping() && over_val.is_mapping()) {
                result.at(key) = merge_map_(result.at(key), over_val);
            } else {
                result[key] = over_val;
            }
        }
        return result;
    }

    /**
     * @brief Merges an override components[] list onto a base components[]
     *        list, matched by (normalized type/tag, id).
     */
    static fkyaml::node merge_components_(const fkyaml::node& base_seq, const fkyaml::node& over_seq) {
        fkyaml::node result = base_seq.is_sequence() ? base_seq : fkyaml::node::sequence();

        for (const auto& over_comp : over_seq) {
            std::string tag = component_tag_(over_comp);
            std::string id = (over_comp.is_mapping() && over_comp.contains("id"))
                ? over_comp.at("id").get_value<std::string>() : "";

            std::vector<std::size_t> matches;
            auto& seq = result.as_seq();
            for (std::size_t i = 0; i < seq.size(); ++i) {
                if (component_tag_(seq[i]) != tag) continue;
                std::string base_id = (seq[i].is_mapping() && seq[i].contains("id"))
                    ? seq[i].at("id").get_value<std::string>() : "";
                if (base_id == id) matches.push_back(i);
            }

            if (truthy_(over_comp, "remove")) {
                for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
                    seq.erase(seq.begin() + static_cast<long>(*it));
                }
                continue;
            }

            if (matches.size() == 1) {
                fkyaml::node merged = merge_map_(seq[matches[0]], over_comp);
                // An Animator's `states` is the one list field matched entry-wise (by `name`),
                // so a variant rig can retarget or add a single clip without restating the set.
                if (tag == "Animator" && seq[matches[0]].is_mapping() &&
                    seq[matches[0]].contains("states") && over_comp.is_mapping() &&
                    over_comp.contains("states")) {
                    merged["states"] = merge_states_(seq[matches[0]].at("states"), over_comp.at("states"));
                }
                erase_key_(merged, "remove");
                seq[matches[0]] = merged;
            } else {
                if (matches.size() > 1) {
                    coopa::debug::Logger("SceneInheritance").warn(
                        "ambiguous component override for type '" + tag +
                        "' (matches " + std::to_string(matches.size()) +
                        " base entries); appending instead of merging. Add an 'id:' to disambiguate.");
                }
                fkyaml::node appended = over_comp;
                erase_key_(appended, "remove");
                seq.push_back(appended);
            }
        }
        return result;
    }

    /**
     * @brief Merges an override Animator `states[]` list onto a base one, matched by `name`,
     *        preserving base ordering.
     *
     * The single exception to "sequences replace wholesale" (see this file's merge rules).
     * A state's `name` is its identity: a matched entry deep-merges, so re-pointing one
     * `clip` is a one-key override and every other state keeps coming from the base.
     * Unmatched entries append; `remove: true` drops a state the base declared.
     *
     * Two base states sharing a `name` is a malformed Animator — Animator::find_state()
     * takes the first at runtime — so this merges into the first and warns, rather
     * than appending a third entry none of the three would win.
     */
    static fkyaml::node merge_states_(const fkyaml::node& base_seq, const fkyaml::node& over_seq) {
        if (!over_seq.is_sequence()) return over_seq;
        fkyaml::node result = base_seq.is_sequence() ? base_seq : fkyaml::node::sequence();
        auto& seq = result.as_seq();

        for (const auto& over_state : over_seq) {
            const std::string name = (over_state.is_mapping() && over_state.contains("name"))
                ? over_state.at("name").get_value<std::string>() : "";

            long match = -1;
            int hits = 0;
            if (!name.empty()) {
                for (std::size_t i = 0; i < seq.size(); ++i) {
                    if (!seq[i].is_mapping() || !seq[i].contains("name")) continue;
                    if (seq[i].at("name").get_value<std::string>() != name) continue;
                    ++hits;
                    if (match < 0) match = static_cast<long>(i);
                }
            }
            if (hits > 1) {
                coopa::debug::Logger("SceneInheritance").warn(
                    "the base Animator declares " + std::to_string(hits) + " states named '" + name +
                    "'; merging the override into the first (give each state a unique name)");
            }

            if (truthy_(over_state, "remove")) {
                for (std::size_t i = seq.size(); i-- > 0;) {
                    if (seq[i].is_mapping() && seq[i].contains("name") &&
                        seq[i].at("name").get_value<std::string>() == name) {
                        seq.erase(seq.begin() + static_cast<long>(i));
                    }
                }
                continue;
            }

            if (match >= 0) {
                fkyaml::node merged = merge_map_(seq[static_cast<std::size_t>(match)], over_state);
                erase_key_(merged, "remove");
                seq[static_cast<std::size_t>(match)] = merged;
            } else {
                fkyaml::node appended = over_state;
                erase_key_(appended, "remove");
                seq.push_back(appended);
            }
        }
        return result;
    }

    /**
     * @brief Merges an override children[] list onto a base children[] list,
     *        matched by name, preserving base ordering.
     */
    static fkyaml::node merge_children_(const fkyaml::node& base_seq, const fkyaml::node& over_seq) {
        fkyaml::node result = base_seq.is_sequence() ? base_seq : fkyaml::node::sequence();
        auto& seq = result.as_seq();

        for (const auto& over_child : over_seq) {
            std::string name = (over_child.is_mapping() && over_child.contains("name"))
                ? over_child.at("name").get_value<std::string>() : "";

            long match = -1;
            if (!name.empty()) {
                for (std::size_t i = 0; i < seq.size(); ++i) {
                    if (seq[i].is_mapping() && seq[i].contains("name") &&
                        seq[i].at("name").get_value<std::string>() == name) {
                        match = static_cast<long>(i);
                        break;
                    }
                }
            }

            if (truthy_(over_child, "remove")) {
                if (match >= 0) seq.erase(seq.begin() + match);
                continue;
            }

            if (match >= 0) {
                fkyaml::node merged = merge_object_(seq[static_cast<std::size_t>(match)], over_child);
                seq[static_cast<std::size_t>(match)] = merged;
            } else {
                fkyaml::node appended = over_child;
                erase_key_(appended, "remove");
                seq.push_back(appended);
            }
        }
        return result;
    }

    /** @brief Merges an override object node onto a base object node. */
    static fkyaml::node merge_object_(const fkyaml::node& base, const fkyaml::node& over) {
        fkyaml::node result = merge_map_(base, over);

        if (over.contains("components")) {
            result["components"] = merge_components_(
                base.contains("components") ? base.at("components") : fkyaml::node::sequence(),
                over.at("components"));
        }
        if (over.contains("children")) {
            result["children"] = merge_children_(
                base.contains("children") ? base.at("children") : fkyaml::node::sequence(),
                over.at("children"));
        }
        erase_key_(result, "inherit_from");
        return result;
    }

    /** @brief Merges an override `scene:` block onto a base `scene:` block. */
    static fkyaml::node merge_scene_(const fkyaml::node& base, const fkyaml::node& over) {
        fkyaml::node result = merge_map_(base, over);
        if (over.contains("root_objects")) {
            result["root_objects"] = merge_children_(
                base.contains("root_objects") ? base.at("root_objects") : fkyaml::node::sequence(),
                over.at("root_objects"));
        }
        erase_key_(result, "inherit_from");
        return result;
    }

    // ---- extracting a usable base object out of a resolved document ----

    static fkyaml::node* find_named_(fkyaml::node& obj, const std::string& name) {
        if (obj.is_mapping() && obj.contains("name") && obj.at("name").get_value<std::string>() == name) {
            return &obj;
        }
        if (obj.is_mapping() && obj.contains("children")) {
            for (auto& child : obj.at("children").as_seq()) {
                if (fkyaml::node* found = find_named_(child, name)) return found;
            }
        }
        return nullptr;
    }

    /** @brief Picks the base object node out of a fully-resolved base document, per the file-shape rules. */
    static fkyaml::node extract_object_(fkyaml::node& doc, const std::string& anchor_name, const std::string& base_path) {
        if (doc.contains("object")) return doc.at("object");

        if (doc.contains("scene")) {
            fkyaml::node& scene_node = doc.at("scene");
            if (!scene_node.contains("root_objects") || scene_node.at("root_objects").size() == 0) {
                throw std::runtime_error("[SceneInheritance] Inherited scene has no root_objects to use as a base object: " + base_path);
            }
            if (!anchor_name.empty()) {
                for (auto& obj : scene_node.at("root_objects").as_seq()) {
                    if (fkyaml::node* found = find_named_(obj, anchor_name)) return *found;
                }
                throw std::runtime_error("[SceneInheritance] Object '" + anchor_name + "' not found in inherited scene: " + base_path);
            }
            return scene_node.at("root_objects")[0];
        }

        return doc; // bare top-level mapping used as-is
    }

    // ---- recursive resolution ----

    /** @brief Resolves inherit_from on this object's children first (post-order), then on the object itself. */
    static void resolve_object_inherit_(
        fkyaml::node& obj,
        const DocumentLoader& load_document,
        std::vector<std::string>& stack,
        const std::string& declaring_dir,
        const std::string& root_dir)
    {
        if (!obj.is_mapping()) return;

        if (obj.contains("children") && obj.at("children").is_sequence()) {
            for (auto& child : obj.at("children").as_seq()) {
                resolve_object_inherit_(child, load_document, stack, declaring_dir, root_dir);
            }
        }

        const bool is_prefab = obj.contains("prefab");
        if (!obj.contains("inherit_from") && !is_prefab) return;
        std::vector<std::string> refs = as_string_list_(obj.at(is_prefab ? "prefab" : "inherit_from"));

        fkyaml::node merged_base;
        bool have_base = false;
        for (const auto& ref : refs) {
            auto anchor_split = split_anchor_(ref);
            std::string base_path = resolve_inherit_path_(anchor_split.first, declaring_dir, root_dir);
            fkyaml::node base_doc = resolve_document_(base_path, load_document, stack, root_dir);
            fkyaml::node base_obj = extract_object_(base_doc, anchor_split.second, base_path);
            merged_base = have_base ? merge_object_(merged_base, base_obj) : base_obj;
            have_base = true;
        }

        if (have_base && is_prefab) {
            // A prefab instance places itself: its own root Transform replaces the prefab's --
            // and likewise its own RectTransform, a UI prefab's placement on its canvas.
            for (const char* placement : {"Transform", "RectTransform"}) {
                bool own = false;
                if (obj.contains("components") && obj.at("components").is_sequence()) {
                    for (const auto& c : obj.at("components")) own |= component_tag_(c) == placement;
                }
                if (own && merged_base.contains("components") && merged_base.at("components").is_sequence()) {
                    auto& seq = merged_base.at("components").as_seq();
                    seq.erase(std::remove_if(seq.begin(), seq.end(),
                                             [placement](const fkyaml::node& c) { return component_tag_(c) == placement; }),
                              seq.end());
                }
            }
        }
        if (have_base) obj = merge_object_(merged_base, obj);
        else erase_key_(obj, "inherit_from");
        erase_key_(obj, "prefab");
    }

    /** @brief Loads `path`, expands every inherit_from reachable from it, and returns the merged document. */
    static fkyaml::node resolve_document_(
        const std::string& path,
        const DocumentLoader& load_document,
        std::vector<std::string>& stack,
        const std::string& root_dir)
    {
        std::string canon = canonicalize_(path);
        if (std::find(stack.begin(), stack.end(), canon) != stack.end()) {
            std::ostringstream oss;
            oss << "[SceneInheritance] inherit_from cycle detected: ";
            for (const auto& s : stack) oss << s << " -> ";
            oss << path;
            throw std::runtime_error(oss.str());
        }
        if (stack.size() >= static_cast<std::size_t>(kMaxDepth)) {
            throw std::runtime_error(
                "[SceneInheritance] inherit_from chain exceeds max depth (" +
                std::to_string(kMaxDepth) + ") while loading: " + path);
        }
        stack.push_back(canon);

        fkyaml::node doc = load_document(path);
        std::string dir = parent_dir_(path);

        // Stamp provenance and resolve object-level inherit_from, depth-first,
        // local document first. Stamping walks the actual object nodes
        // (root_objects / object), not the document's own format/scene/object
        // keys, which carry no components/children of their own.
        if (doc.contains("scene") && doc.at("scene").contains("root_objects")) {
            for (auto& obj : doc.at("scene").at("root_objects").as_seq()) {
                stamp_source_dir_(obj, dir);
                resolve_object_inherit_(obj, load_document, stack, dir, root_dir);
            }
        }
        if (doc.contains("object")) {
            stamp_source_dir_(doc.at("object"), dir);
            resolve_object_inherit_(doc.at("object"), load_document, stack, dir, root_dir);
        }

        // Step 4: scene-level inherit_from.
        if (doc.contains("scene") && doc.at("scene").contains("inherit_from")) {
            std::vector<std::string> refs = as_string_list_(doc.at("scene").at("inherit_from"));

            fkyaml::node merged_scene;
            bool have_base = false;
            for (const auto& ref : refs) {
                auto anchor_split = split_anchor_(ref);
                std::string base_path = resolve_inherit_path_(anchor_split.first, dir, root_dir);
                fkyaml::node base_doc = resolve_document_(base_path, load_document, stack, root_dir);
                if (!base_doc.contains("scene")) {
                    throw std::runtime_error(
                        "[SceneInheritance] scene-level inherit_from target has no 'scene:' block: " + base_path);
                }
                merged_scene = have_base ? merge_scene_(merged_scene, base_doc.at("scene")) : base_doc.at("scene");
                have_base = true;
            }
            if (have_base) doc.at("scene") = merge_scene_(merged_scene, doc.at("scene"));
        }

        stack.pop_back();
        return doc;
    }
};

} // namespace scene
} // namespace coopa

#endif // COOPA_SCENE_SCENE_INHERIT_H
