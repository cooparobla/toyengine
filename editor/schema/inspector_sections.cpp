#include "editor/schema/inspector_sections.h"

#include <map>

namespace toy {
namespace editor {

namespace {

std::map<std::string, SectionDrawFn>& drawers() {
    static std::map<std::string, SectionDrawFn> table;
    return table;
}

} // namespace

bool register_inspector_section(std::string id, SectionDrawFn fn) {
    drawers()[std::move(id)] = std::move(fn);
    return true;
}

void unregister_inspector_section(const std::string& id) { drawers().erase(id); }

const SectionDrawFn* find_inspector_section(const std::string& id) {
    if (id.empty()) return nullptr;
    auto it = drawers().find(id);
    return it == drawers().end() ? nullptr : &it->second;
}

} // namespace editor
} // namespace toy
