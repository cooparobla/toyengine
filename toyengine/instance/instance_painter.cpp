#include <toyengine/instance/instance_painter.h>

#include <algorithm>
#include <filesystem>
#include <limits>

#include <coopa/scene/scene_loader.h>
#include <gfxcoopa/engine/data/mesh.h>

namespace toy {
namespace instance {

namespace {

using coopa::gfx::engine::components::MeshRenderer;

/** @brief Every component type a painted instance cannot carry, with no per-instance object. */
const char* const k_behaviour_components[] = {
    "Animator", "ParticleSystem", "CharacterController", "Rigidbody", "Cloth",
    "Ragdoll", "KinematicMover", "KinematicController", "Vehicle", "Canvas",
};

void collect_parts_(coopa::scene::SceneObject& obj, const glm::mat4& parent,
                    std::vector<InstancePrototype::Part>& out, std::vector<std::string>& behaviours) {
    glm::mat4 local = parent;
    if (auto* tc = obj.get_transform()) local = parent * tc->transform().get_local_matrix();
    for (const char* type : k_behaviour_components) {
        if (obj.get_component_by_type_name(type) != nullptr) behaviours.push_back(type);
    }
    if (auto* mr = obj.get_component<MeshRenderer>()) {
        if (!mr->mesh_path().empty()) out.push_back({mr, local});
    }
    for (const auto& child : obj.children()) collect_parts_(*child, local, out, behaviours);
}

} // namespace

bool parse_instance_collision(const std::string& name, InstanceCollision& out) {
    if (name == "none" || name.empty()) { out = InstanceCollision::None; return true; }
    if (name == "auto")    { out = InstanceCollision::Auto; return true; }
    if (name == "box")     { out = InstanceCollision::Box; return true; }
    if (name == "sphere")  { out = InstanceCollision::Sphere; return true; }
    if (name == "capsule") { out = InstanceCollision::Capsule; return true; }
    if (name == "mesh")    { out = InstanceCollision::Mesh; return true; }
    return false;
}

const char* instance_collision_name(InstanceCollision c) {
    switch (c) {
        case InstanceCollision::None:    return "none";
        case InstanceCollision::Auto:    return "auto";
        case InstanceCollision::Box:     return "box";
        case InstanceCollision::Sphere:  return "sphere";
        case InstanceCollision::Capsule: return "capsule";
        case InstanceCollision::Mesh:    return "mesh";
    }
    return "none";
}

std::string InstancePrototype::label() const {
    if (!name.empty()) return name;
    const std::string& ref = !object.empty() ? object : mesh;
    const std::size_t slash = ref.find_last_of('/');
    return slash == std::string::npos ? ref : ref.substr(slash + 1);
}

bool InstancePrototype::ready() const {
    if (parts.empty()) return false;
    for (const Part& p : parts) {
        if (!p.proxy || !p.proxy->is_ready()) return false;
    }
    return true;
}

float InstancePrototype::nominal_height() const {
    const float h = bounds_max.z - bounds_min.z;
    return std::max(0.05f, h * std::max(scale_range.x, scale_range.y));
}

int InstancePainter::find_prototype(const std::string& name) const {
    for (std::size_t i = 0; i < prototypes.size(); ++i) {
        if (prototypes[i].label() == name) return static_cast<int>(i);
    }
    return -1;
}

bool resolve_prototype(InstancePrototype& p, const std::function<void(const std::string&)>& log_warning) {
    if (p.resolved) return !p.parts.empty();
    p.resolved = true;
    auto warn = [&](const std::string& line) {
        if (p.warned || !log_warning) return;
        p.warned = true;
        log_warning(line);
    };

    if (!p.object.empty()) {
        std::vector<std::string> behaviours;
        try {
            // Through the ordinary prefab path, so nested prefabs, material slots and LODs all
            // come along. Never parented into a scene, never started: it exists to be read.
            p.templ = std::shared_ptr<coopa::scene::SceneObject>(coopa::scene::SceneLoader::instantiate(p.object));
        } catch (const std::exception& e) {
            warn("[InstancePainter] prototype '" + p.object + "' did not load: " + e.what());
            return false;
        }
        collect_parts_(*p.templ, glm::mat4(1.0f), p.parts, behaviours);
        if (p.parts.empty()) {
            warn("[InstancePainter] object asset '" + p.object + "' has no MeshRenderer, so it paints nothing");
            return false;
        }
        if (!behaviours.empty()) {
            std::string list;
            for (const std::string& b : behaviours) list += (list.empty() ? "" : ", ") + b;
            warn("[InstancePainter] '" + p.object + "' carries " + list +
                 ", which a painted instance cannot run (it is a record, not an object). Its meshes "
                 "are drawn; place it with prefab: instances, or Convert to Object, if it must behave.");
        }
    } else if (!p.mesh.empty()) {
        if (!p.own_proxy) {
            warn("[InstancePainter] prototype mesh '" + p.mesh + "' has no proxy: the parser builds it");
            return false;
        }
        p.parts.push_back({p.own_proxy.get(), glm::mat4(1.0f)});
    } else {
        warn("[InstancePainter] a prototype needs either `object:` or `mesh:`");
        return false;
    }
    return !p.parts.empty();
}

void refresh_prototype_bounds(InstancePrototype& p) {
    p.bounds_min = glm::vec3(std::numeric_limits<float>::max());
    p.bounds_max = glm::vec3(std::numeric_limits<float>::lowest());
    bool any = false;
    for (const InstancePrototype::Part& part : p.parts) {
        if (!part.proxy || !part.proxy->is_ready()) continue;
        const auto& mesh = *part.proxy->get_mesh();
        const glm::vec3 lo = mesh.bounds_min(), hi = mesh.bounds_max();
        // Every corner, because the part's local matrix may rotate it.
        for (int c = 0; c < 8; ++c) {
            const glm::vec3 corner((c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z);
            const glm::vec3 w = glm::vec3(part.local * glm::vec4(corner, 1.0f));
            p.bounds_min = glm::min(p.bounds_min, w);
            p.bounds_max = glm::max(p.bounds_max, w);
            any = true;
        }
    }
    if (!any) {
        p.bounds_min = glm::vec3(0.0f);
        p.bounds_max = glm::vec3(0.0f);
    }
}

} // namespace instance
} // namespace toy
