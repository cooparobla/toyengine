#include "editor/schema/item_list.h"

#include "editor/core/yaml_util.h"

#include <cctype>

namespace toy {
namespace editor {
namespace {

/** @brief The item's value for `key`, or "" when it is absent, empty or not text. */
std::string text_at(const Node& item, const std::string& key) {
    if (!item.is_mapping() || !item.contains(key)) return {};
    const Node& n = item.at(key);
    return n.is_string() ? n.get_value<std::string>() : std::string{};
}

bool holds_text(FieldKind k) {
    return k == FieldKind::String || k == FieldKind::AssetRef || k == FieldKind::ChildRef ||
           k == FieldKind::Enum;
}

} // namespace

std::string item_noun(const FieldDesc& list) {
    if (!list.item_noun.empty()) return list.item_noun;
    std::string s = list.display();
    if (s.size() > 1 && s.back() == 's') s.pop_back();
    return s;
}

std::string item_title(const FieldDesc& list, const Node& item, std::size_t index) {
    std::string name;
    if (!list.item_title_key.empty()) name = text_at(item, list.item_title_key);
    if (name.empty()) {
        for (const char* key : {"label", "name", "title", "id"}) {
            name = text_at(item, key);
            if (!name.empty()) break;
        }
    }
    if (name.empty()) {
        for (const auto& sub : list.item_fields) {
            if (!holds_text(sub.kind)) continue;
            name = text_at(item, sub.key);
            if (!name.empty()) break;
        }
    }
    // The ordinal joins a name, and replaces one that had to be invented: numbering a
    // "Layer 3" twice reads as two different numbers for the same row.
    if (name.empty()) return item_noun(list) + " " + std::to_string(index + 1);
    if (!list.items_reorderable) return std::to_string(index) + "  " + name;
    return name;
}

std::string item_list_empty_text(const FieldDesc& list) {
    std::string plural = list.display();
    for (char& c : plural) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return "No " + plural + " yet.";
}

Node make_default_item(const FieldDesc& list, std::size_t index) {
    Node m = Node::mapping();
    for (const auto& sub : list.item_fields) {
        if (sub.kind == FieldKind::Enum && !sub.options.empty()) m[sub.key] = Node(sub.options.front());
        else if (sub.key == "label") m[sub.key] = Node("Item " + std::to_string(index + 1));
        else if (sub.key == "name")  m[sub.key] = Node("Item" + std::to_string(index + 1));
        else if (sub.kind == FieldKind::Bool)  m[sub.key] = sub.def.x != 0.0f;
        else if (sub.kind == FieldKind::Int)   m[sub.key] = static_cast<int>(sub.def.x);
        else if (sub.kind == FieldKind::Float) m[sub.key] = sub.def.x;
        else m[sub.key] = Node(sub.default_string);
    }
    return m;
}

} // namespace editor
} // namespace toy
