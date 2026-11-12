#include <toyengine/save/save_game.h>

namespace toy {
namespace save {
namespace detail {

fkyaml::node vec_node(const float* v, int n) {
    static const char* const k[] = {"x", "y", "z", "w"};
    fkyaml::node out = fkyaml::node::mapping();
    for (int i = 0; i < n; ++i) out[k[i]] = fkyaml::node(static_cast<double>(v[i]));
    return out;
}

bool read_float(const fkyaml::node& n, float& out) {
    if (n.is_float_number()) { out = static_cast<float>(n.get_value<double>()); return true; }
    if (n.is_integer())      { out = static_cast<float>(n.get_value<int64_t>()); return true; }
    return false;
}

bool read_vec(const fkyaml::node& n, float* v, int n_comp) {
    static const char* const k[] = {"x", "y", "z", "w"};
    if (!n.is_mapping()) return false;
    for (int i = 0; i < n_comp; ++i) {
        if (!n.contains(k[i]) || !read_float(n.at(k[i]), v[i])) return false;
    }
    return true;
}

} // namespace detail
} // namespace save
} // namespace toy

namespace toy {
namespace save {

SaveNode SaveNode::section(const std::string& key) {
    if (!writable_) return std::as_const(*this).section(key);
    fkyaml::node& n = mutable_node_();
    if (!n.contains(key) || !n[key].is_mapping()) n[key] = fkyaml::node::mapping();
    return SaveNode(&n[key], true);
}

SaveNode SaveNode::section(const std::string& key) const {
    if (!has(key) || !node_->at(key).is_mapping()) return SaveNode();
    return SaveNode(&(*node_)[key], false);
}

std::vector<std::string> SaveNode::keys() const {
    std::vector<std::string> out;
    if (!valid()) return out;
    for (const auto& [k, v] : node_->as_map()) {
        (void)v;
        if (k.is_string()) out.push_back(k.get_value<std::string>());
    }
    return out;
}

const fkyaml::node& SaveNode::node() const {
    static const fkyaml::node none;
    return node_ ? *node_ : none;
}

fkyaml::node& SaveNode::mutable_node_() {
    if (!writable_ || !node_) throw std::logic_error("SaveNode: writing through a read-only save section");
    if (!node_->is_mapping()) *node_ = fkyaml::node::mapping();
    return *node_;
}

int SaveGame::version() const {
    return root_.contains("version") && root_.at("version").is_integer()
               ? static_cast<int>(root_.at("version").get_value<int64_t>()) : 0;
}

void SaveGame::normalize_() {
    if (!root_.is_mapping()) root_ = fkyaml::node::mapping();
    for (const char* k : {"meta", "global", "objects"}) {
        if (!root_.contains(k) || !root_[k].is_mapping()) root_[k] = fkyaml::node::mapping();
    }
    if (!root_.contains("version")) set_version(0);
}

} // namespace save
} // namespace toy
