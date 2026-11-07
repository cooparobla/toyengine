#include "editor/rig/rig_model.h"

#include <coopa/rig/rig_asset.h>

#include <algorithm>
#include <cmath>
#include <functional>

namespace toy {
namespace editor {

namespace {

coopa::rig::Bone to_coopa_bone(const RigModelBone& b) {
    coopa::rig::Bone out;
    out.name = b.name;
    out.parent = b.parent;
    out.head = b.head;
    out.tail = b.tail;
    out.roll = b.roll;
    out.connected = b.connected;
    return out;
}

} // namespace

RigModel RigModel::from_node(const Node& root) {
    RigModel out;
    if (!root.contains("rig")) return out;
    const Node& node = root.at("rig");
    if (node.contains("name")) out.name = node.at("name").get_value<std::string>();
    if (!node.contains("bones") || !node.at("bones").is_sequence()) return out;
    for (const Node& bn : node.at("bones").as_seq()) {
        RigModelBone b;
        if (bn.contains("name")) b.name = bn.at("name").get_value<std::string>();
        if (bn.contains("head")) b.head = as_vec3(bn.at("head"));
        if (bn.contains("tail")) b.tail = as_vec3(bn.at("tail"));
        if (bn.contains("roll")) b.roll = bn.at("roll").get_value<float>();
        if (bn.contains("connected")) b.connected = bn.at("connected").get_value<bool>();
        if (bn.contains("parent")) {
            const std::string parent_name = bn.at("parent").get_value<std::string>();
            for (size_t i = 0; i < out.bones.size(); ++i) {
                if (out.bones[i].name == parent_name) {
                    b.parent = static_cast<int>(i);
                    break;
                }
            }
        }
        out.bones.push_back(b);
    }
    return out;
}

Node RigModel::to_node() const {
    Node root = Node::mapping();
    Node rig = Node::mapping();
    rig["name"] = Node(name);
    Node bones_node = Node::sequence();
    for (const RigModelBone& b : bones) {
        Node bn = Node::mapping();
        bn["name"] = Node(b.name);
        if (b.parent >= 0 && static_cast<size_t>(b.parent) < bones.size()) bn["parent"] = Node(bones[static_cast<size_t>(b.parent)].name);
        bn["head"] = make_vec3(b.head);
        bn["tail"] = make_vec3(b.tail);
        if (b.roll != 0.0f) bn["roll"] = make_float(b.roll);
        if (b.connected) bn["connected"] = Node(true);
        bones_node.as_seq().push_back(bn);
    }
    rig["bones"] = bones_node;
    root["rig"] = rig;
    return root;
}

int RigModel::find_bone(const std::string& bone_name) const {
    for (size_t i = 0; i < bones.size(); ++i) if (bones[i].name == bone_name) return static_cast<int>(i);
    return -1;
}

glm::mat4 RigModel::local_rest_matrix(int i) const {
    const glm::mat4 w = world_rest_matrix(i);
    const int p = bones[static_cast<size_t>(i)].parent;
    return p >= 0 ? glm::inverse(world_rest_matrix(p)) * w : w;
}

glm::mat4 RigModel::world_rest_matrix(int i) const {
    // head/tail are already in rig space (see the asset's own doc), so a bone's rest basis is
    // derived from itself alone -- no parent composition needed for position or orientation.
    return coopa::rig::bone_rest_matrix(to_coopa_bone(bones[static_cast<size_t>(i)]));
}

int RigModel::add_bone(const std::string& bone_name, int parent, const glm::vec3& head, const glm::vec3& tail) {
    RigModelBone b;
    b.name = bone_name;
    b.parent = parent;
    b.head = head;
    b.tail = tail;
    bones.push_back(b);
    return static_cast<int>(bones.size()) - 1;
}

int RigModel::extrude_bone(int from, const std::string& bone_name, const glm::vec3& new_tail) {
    if (from < 0 || static_cast<size_t>(from) >= bones.size()) return -1;
    RigModelBone b;
    b.name = bone_name;
    b.parent = from;
    b.head = bones[static_cast<size_t>(from)].tail;
    b.tail = new_tail;
    b.connected = true;
    bones.push_back(b);
    return static_cast<int>(bones.size()) - 1;
}

void RigModel::set_head(int i, const glm::vec3& head) {
    RigModelBone& b = bones[static_cast<size_t>(i)];
    if (b.connected && b.parent >= 0) {
        set_tail(b.parent, head);   // the same point on screen -- move the parent's tail instead
        return;
    }
    b.head = head;
}

void RigModel::set_tail(int i, const glm::vec3& tail) {
    bones[static_cast<size_t>(i)].tail = tail;
    for (size_t j = 0; j < bones.size(); ++j) {
        if (bones[j].connected && bones[j].parent == i) bones[j].head = tail;
    }
}

void RigModel::set_roll(int i, float degrees) { bones[static_cast<size_t>(i)].roll = degrees; }

bool RigModel::rename_bone(int i, const std::string& to) {
    if (to.empty() || i < 0 || static_cast<size_t>(i) >= bones.size()) return false;
    for (size_t j = 0; j < bones.size(); ++j) {
        if (static_cast<int>(j) != i && bones[j].name == to) return false;
    }
    bones[static_cast<size_t>(i)].name = to;
    return true;
}

bool RigModel::reparent_bone(int i, int new_parent) {
    if (i < 0 || static_cast<size_t>(i) >= bones.size()) return false;
    if (new_parent == i) return false;
    if (new_parent >= i) return false;   // parent-first order: only an EARLIER bone may be the parent
    // A cycle can only happen via a bone whose parent chain already passes through i -- impossible
    // once new_parent < i given the existing list is already parent-first, but guard anyway.
    for (int p = new_parent; p >= 0;) {
        if (p == i) return false;
        p = bones[static_cast<size_t>(p)].parent;
    }
    bones[static_cast<size_t>(i)].parent = new_parent;
    bones[static_cast<size_t>(i)].connected = false;   // a new parent's tail is a different point; never silently reconnect
    return true;
}

void RigModel::set_connected(int i, bool on) {
    RigModelBone& b = bones[static_cast<size_t>(i)];
    if (on && b.parent >= 0) {
        b.head = bones[static_cast<size_t>(b.parent)].tail;
        b.connected = true;
    } else {
        b.connected = on;
    }
}

std::vector<std::string> RigModel::delete_bone(int i) {
    std::vector<std::string> reparented;
    if (i < 0 || static_cast<size_t>(i) >= bones.size()) return reparented;
    const int grandparent = bones[static_cast<size_t>(i)].parent;
    const glm::vec3 pin = grandparent >= 0 ? bones[static_cast<size_t>(grandparent)].tail : bones[static_cast<size_t>(i)].head;
    for (size_t j = 0; j < bones.size(); ++j) {
        if (static_cast<int>(j) == i || bones[j].parent != i) continue;
        reparented.push_back(bones[j].name);
        bones[j].parent = grandparent;
        if (bones[j].connected) {
            if (grandparent >= 0) bones[j].head = pin;
            else bones[j].connected = false;   // no parent to pin to
        }
    }
    bones.erase(bones.begin() + i);
    for (RigModelBone& b : bones) {
        if (b.parent == i) b.parent = grandparent;
        else if (b.parent > i) --b.parent;
    }
    return reparented;
}

std::vector<int> RigModel::sort_parent_first() {
    const int n = static_cast<int>(bones.size());
    std::vector<std::vector<int>> children(static_cast<size_t>(n));
    std::vector<int> roots;
    for (int i = 0; i < n; ++i) {
        const int p = bones[static_cast<size_t>(i)].parent;
        if (p >= 0 && p < n && p != i) children[static_cast<size_t>(p)].push_back(i);
        else roots.push_back(i);
    }
    // Depth first from each root, children in their existing relative order: a stable result, so
    // sorting an already-sorted rig is the identity and re-saving it is byte-stable.
    std::vector<int> order;
    order.reserve(bones.size());
    std::vector<char> seen(static_cast<size_t>(n), 0);
    const std::function<void(int)> walk = [&](int i) {
        if (seen[static_cast<size_t>(i)]) return;   // a parent cycle would otherwise spin forever
        seen[static_cast<size_t>(i)] = 1;
        order.push_back(i);
        for (int c : children[static_cast<size_t>(i)]) walk(c);
    };
    for (int r : roots) walk(r);
    for (int i = 0; i < n; ++i) walk(i);   // anything left is in a cycle: keep it rather than drop it

    std::vector<int> old_to_new(static_cast<size_t>(n), -1);
    for (size_t k = 0; k < order.size(); ++k) old_to_new[static_cast<size_t>(order[k])] = static_cast<int>(k);
    std::vector<RigModelBone> out;
    out.reserve(bones.size());
    for (int old : order) out.push_back(bones[static_cast<size_t>(old)]);
    for (RigModelBone& b : out) {
        if (b.parent >= 0 && b.parent < n) b.parent = old_to_new[static_cast<size_t>(b.parent)];
    }
    bones = std::move(out);
    return old_to_new;
}

float roll_to_align_z(const glm::vec3& head, const glm::vec3& tail, const glm::vec3& up) {
    const glm::vec3 axis = tail - head;
    if (glm::length(axis) < 1e-8f) return 0.0f;
    const glm::vec3 y = glm::normalize(axis);
    // The roll-0 basis, straight from the runtime's own builder so the convention cannot drift
    // (including its seed-axis switch for a bone running along world Z).
    RigModelBone probe;
    probe.head = head;
    probe.tail = tail;
    probe.roll = 0.0f;
    const glm::mat4 m = coopa::rig::bone_rest_matrix(to_coopa_bone(probe));
    const glm::vec3 x0(m[0]), z0(m[2]);
    const glm::vec3 perp = up - y * glm::dot(up, y);   // no roll can tilt out of this plane
    if (glm::length(perp) < 1e-6f) return 0.0f;        // `up` is along the bone: nothing to aim at
    return glm::degrees(std::atan2(glm::dot(perp, x0), glm::dot(perp, z0)));
}

} // namespace editor
} // namespace toy
