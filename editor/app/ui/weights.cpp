#include "editor/app/editor_app.h"

#include "editor/mesh/auto_weights.h"
#include "editor/schema/component_schema.h"

#include <toyengine/scene/skinned_mesh_renderer.h>

#include <algorithm>

namespace toy {
namespace editor {

namespace {

/** @brief The last component of a bone path ("Hips/Spine" -> "Spine"), which names the object. */
std::string bone_leaf(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

} // namespace

bool EditorApp::bind_targets_(ObjectId& skin, ObjectId& rig) {
    skin = rig = 0;
    if (playing() || asset_view_()) return false;

    // The active object first, so selecting mesh + rig binds the mesh whichever went last.
    std::vector<ObjectId> candidates;
    if (doc_.primary()) candidates.push_back(doc_.primary());
    for (ObjectId id : doc_.selection()) if (id != doc_.primary()) candidates.push_back(id);
    for (ObjectId id : candidates) {
        const Node* n = doc_.find(id);
        if (n && !object_mesh_key_(effective_(*n)).empty()) { skin = id; break; }
    }
    if (!skin) return false;

    for (ObjectId id : candidates) {
        if (id != skin && doc_.is_ancestor(id, skin)) { rig = id; break; }
    }
    if (!rig) rig = rig_of_(skin);
    if (!rig) {
        if (const std::optional<ObjectId> parent = doc_.parent_of(skin)) rig = *parent;
    }
    if (!rig || rig == skin) { rig = 0; return false; }

    // A rig with no joint under it has nothing to deform with.
    for (const auto& [id, depth] : rig_objects_(rig)) {
        if (id != rig && id != skin && is_joint_object_(id)) return true;
    }
    rig = 0;
    return false;
}

std::vector<std::string> EditorApp::skinned_bone_names_(ObjectId skin) {
    std::vector<std::string> out;
    const Node* n = doc_.find(skin);
    if (!n) return out;
    const Node& eff = effective_(*n);
    if (!eff.contains("components") || !eff.at("components").is_sequence()) return out;
    for (const auto& c : eff.at("components").as_seq()) {
        if (component_type(c) != "SkinnedMeshRenderer" || !c.contains("bones")) continue;
        if (!c.at("bones").is_sequence()) continue;
        for (const auto& b : c.at("bones").as_seq()) {
            if (b.is_string()) out.push_back(bone_leaf(b.get_value<std::string>()));
        }
    }
    return out;
}

std::vector<BoneSegments> EditorApp::rig_bone_segments_(ObjectId rig, ObjectId skin) {
    std::vector<BoneSegments> out;
    if (!rig || !skin) return out;
    // The rest pose is the bind pose, so a rig the Timeline has posed goes back to rest first.
    if (anim_posed_ && anim_rig_ == rig) { anim_restore_rest_pose_(); anim_posed_ = false; }

    const coopa::scene::SceneObject* skin_live = sync_.live(skin);
    if (!skin_live || !skin_live->get_transform()) return out;
    const glm::mat4 to_mesh = glm::inverse(skin_live->get_transform()->transform().get_world_matrix());
    auto into_mesh = [&](const glm::vec3& world) { return glm::vec3(to_mesh * glm::vec4(world, 1.0f)); };

    const std::vector<std::string> only = skinned_bone_names_(skin);
    for (const auto& [id, depth] : rig_objects_(rig)) {
        if (id == rig || id == skin || !is_joint_object_(id)) continue;
        const Node* node = doc_.find(id);
        const std::string name = node ? get_string(*node, "name") : std::string();
        if (name.empty()) continue;
        if (!only.empty() && std::find(only.begin(), only.end(), name) == only.end()) continue;
        const coopa::scene::SceneObject* live = sync_.live(id);
        if (!live || !live->get_transform()) continue;
        const glm::vec3 head(live->get_transform()->transform().get_world_matrix()[3]);
        BoneSegments bone;
        bone.name = name;
        for (const glm::vec3& tail : bone_tails_(id, origin_size_for_(id) * 2.0f)) {
            bone.segments.push_back({into_mesh(head), into_mesh(tail)});
        }
        if (!bone.segments.empty()) out.push_back(std::move(bone));
    }
    return out;
}

void EditorApp::report_bind_(const AutoWeightReport& report, const std::string& rig_name, size_t bones) {
    log_info("Bound to " + rig_name + ": " + std::to_string(bones) + " bone(s), " +
             std::to_string(report.weighted_vertices) + " vertex/vertices weighted");
    if (report.fell_back_to_envelope) {
        log_info("...the mesh has no surface to spread weights over, so envelopes were used instead");
    }
    if (!report.unsolved.empty()) {
        std::string names;
        for (const std::string& n : report.unsolved) names += (names.empty() ? "" : ", ") + n;
        log_error("Automatic Weights: no solution for " + names + " -- those bones got no weights");
    }
    if (!report.foreign_groups.empty()) {
        std::string names;
        for (const std::string& n : report.foreign_groups) names += (names.empty() ? "" : ", ") + n;
        log_info("...the mesh also has vertex groups no bone claims (" + names +
                 "); their weights still count toward each vertex's total");
    }
}

bool EditorApp::bind_to_rig_(ObjectId skin, ObjectId rig, BindWeights kind) {
    if (playing() || asset_view_() || edit_object_) return false;
    if (in_pose_mode_()) {
        log_error("Bind to Rig: leave Pose Mode first -- a bind is made at the rig's rest pose");
        return false;
    }
    const Node* obj = doc_.find(skin);
    if (!obj) return false;
    const std::string key = object_mesh_key_(effective_(*obj));
    if (key.empty()) { log_error("Bind to Rig: that object has no mesh to bind"); return false; }
    const fs::path path = resolve_mesh_key_(key);
    if (!coopa::yaml::document_exists(path) || refuse_engine_asset_(path)) return false;

    const std::vector<BoneSegments> bones = rig_bone_segments_(rig, skin);
    if (bones.empty()) { log_error("Bind to Rig: " + anim_name_of_(rig, "the rig") + " has no bones to deform with"); return false; }
    if (!activate_scene_mesh_(path)) return false;

    AutoWeightSettings settings = auto_weights_;
    settings.method = (kind == BindWeights::Envelope) ? AutoWeightMethod::Envelope : AutoWeightMethod::Heat;
    AutoWeightReport report;
    const char* label = (kind == BindWeights::EmptyGroups) ? "Bind to Rig (Empty Groups)"
                      : (kind == BindWeights::Envelope)   ? "Bind to Rig (Envelope Weights)"
                                                          : "Bind to Rig (Automatic Weights)";
    mesh_.edit(label, [&](EditMesh& m, MeshSelection&) {
        report = (kind == BindWeights::EmptyGroups) ? add_empty_bone_groups(m, bones)
                                                    : apply_auto_weights(m, bones, settings);
    });
    commit_scene_mesh_();
    ensure_skinned_renderer_(skin, rig, key);
    release_static_mesh_(skin, path);
    report_bind_(report, anim_name_of_(rig, "the rig"), bones.size());
    return true;
}

void EditorApp::ensure_skinned_renderer_(ObjectId skin, ObjectId rig, const std::string& mesh_key) {
    const Node* obj = doc_.find(skin);
    if (!obj) return;
    // `rig:` is only written when neither of the engine's own fallbacks would land on this rig:
    // the nearest Animator above the skin, or the skin's topmost ancestor.
    std::string rig_path;
    if (rig_of_(skin) != rig) {
        ObjectId top = skin;
        for (std::optional<ObjectId> cur = doc_.parent_of(skin); cur && *cur != 0; cur = doc_.parent_of(*cur)) top = *cur;
        if (top != rig) rig_path = rig_path_of_(0, rig);
    }

    const int smr = doc_.find_component(skin, "SkinnedMeshRenderer");
    if (smr >= 0) {
        Node comp = obj->at("components").as_seq()[static_cast<size_t>(smr)];
        comp["mesh_path"] = Node(mesh_key);
        if (rig_path.empty()) erase_key(comp, "rig"); else comp["rig"] = Node(rig_path);
        apply_(doc_.set_component(skin, smr, comp, "Bind to Rig"));
    } else {
        Node comp = default_component("SkinnedMeshRenderer");
        comp["mesh_path"] = Node(mesh_key);
        if (!rig_path.empty()) comp["rig"] = Node(rig_path);
        apply_(doc_.add_component(skin, comp));
    }
    // The skinned mesh is the SkinnedMeshRenderer's; its MeshRenderer supplies the material and
    // the draw alone (ClothRenderer's convention), so a leftover mesh_path there would draw the
    // undeformed bind pose over the top.
    const int mr = doc_.find_component(skin, "MeshRenderer");
    if (mr >= 0) {
        const Node& existing = doc_.find(skin)->at("components").as_seq()[static_cast<size_t>(mr)];
        if (!get_string(existing, "mesh_path").empty()) {
            Node comp = existing;
            erase_key(comp, "mesh_path");
            apply_(doc_.set_component(skin, mr, comp, "Bind to Rig"));
        }
    } else {
        apply_(doc_.add_component(skin, default_component("MeshRenderer")));
    }
}

void EditorApp::release_static_mesh_(ObjectId skin, const fs::path& path) {
    if (const auto* live = sync_.live(skin)) {
        // Only an object drawing the file STATICALLY: one already being skinned keeps its
        // dynamic mesh on that same MeshRenderer, and only its SkinnedMeshRenderer puts it back.
        const bool already_skinned = live->get_component<toy::scene::SkinnedMeshRenderer>() != nullptr;
        if (!already_skinned) {
            if (auto* mr = live->get_component<coopa::gfx::engine::components::MeshRenderer>()) {
                mr->set_mesh({});
            }
        }
    }
    mesh_cache_.clear();
    mesh_resolve_cache_.clear();
    engine_.assets().unload(coopa::asset::AssetId::from_path(path.string()));
}

bool EditorApp::regenerate_weights_from_rig_(BindWeights kind) {
    if (playing() || !mesh_.open()) return false;
    const ObjectId skin = edit_object_;
    if (!skin) {
        log_error("Automatic Weights: open the mesh from its object in the scene -- the rig under it is where the weights come from");
        return false;
    }
    ObjectId rig = rig_of_(skin);
    if (!rig) {
        if (const std::optional<ObjectId> parent = doc_.parent_of(skin)) rig = *parent;
    }
    const std::vector<BoneSegments> bones = rig ? rig_bone_segments_(rig, skin) : std::vector<BoneSegments>{};
    if (bones.empty()) { log_error("Automatic Weights: no rig with bones above this mesh"); return false; }

    AutoWeightSettings settings = auto_weights_;
    settings.method = (kind == BindWeights::Envelope) ? AutoWeightMethod::Envelope : AutoWeightMethod::Heat;
    AutoWeightReport report;
    const char* label = (kind == BindWeights::EmptyGroups) ? "Vertex Groups from Rig"
                      : (kind == BindWeights::Envelope)   ? "Envelope Weights from Rig"
                                                          : "Automatic Weights from Rig";
    mesh_.edit(label, [&](EditMesh& m, MeshSelection&) {
        report = (kind == BindWeights::EmptyGroups) ? add_empty_bone_groups(m, bones)
                                                    : apply_auto_weights(m, bones, settings);
    });
    if (active_group_ >= mesh_.mesh.groups.size()) active_group_ = 0;
    report_bind_(report, anim_name_of_(rig, "the rig"), bones.size());
    return true;
}

void EditorApp::draw_auto_weights_panel_(imm::Context& ctx) {
    using I = imm::Icon;
    if (!ctx.collapsing_header("Auto Weights", false, nullptr, I::WeightPaint)) return;
    AutoWeightSettings& s = auto_weights_;
    int method = static_cast<int>(s.method);
    if (ctx.combo("Falloff", &method, {"Bone Heat", "Envelope"})) s.method = static_cast<AutoWeightMethod>(method);
    ctx.tooltip("Falloff\nBone Heat spreads each bone's hold over the surface, so it cannot leak across a gap.\n"
                "Envelope measures straight-line distance instead -- rougher, but it needs no closed surface");
    if (s.method == AutoWeightMethod::Heat) {
        ctx.checkbox("Ignore Hidden Bones", &s.visibility);
        ctx.tooltip("Ignore Hidden Bones\nA bone only weights skin it can be seen from, so a bone passing\n"
                    "close behind a surface does not claim it");
    } else {
        ctx.slider_float("Reach", &s.envelope_falloff, 0.1f, 4.0f, "%.2f");
        ctx.tooltip("Reach\nHow far a bone pulls, as a multiple of its own length");
    }
    ctx.drag_int("Max Influences", &s.max_influences, 0.1f, 1, 8);
    ctx.tooltip("Max Influences\nBones kept per vertex. The engine skins four and ignores the rest");
    ctx.slider_float("Weight Limit", &s.weight_limit, 0.0f, 0.5f, "%.3f");
    ctx.tooltip("Weight Limit\nWeights fainter than this are faded out and dropped");
    ctx.drag_int("Smooth", &s.smooth_iterations, 0.1f, 0, 10);
    ctx.tooltip("Smooth\nBlur passes over the result, for a softer handover at each joint");

    ObjectId skin = 0, rig = 0;
    const bool can_bind = edit_object_ != 0 || bind_targets_(skin, rig);
    if (ctx.button("Generate from Rig", -1, can_bind, I::WeightPaint)) {
        regenerate_weights_from_rig_(s.method == AutoWeightMethod::Envelope ? BindWeights::Envelope
                                                                           : BindWeights::Automatic);
    }
    ctx.tooltip("Generate from Rig\nReplace this mesh's bone vertex groups with freshly generated weights");
    if (!can_bind) ctx.label_dim("Open the mesh from a rigged object to generate weights.");
}

} // namespace editor
} // namespace toy
