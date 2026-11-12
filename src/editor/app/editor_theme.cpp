#include "editor/app/editor_theme.h"

namespace toy {
namespace editor {

EditorTheme editor_theme_from(const coopa::ui::imm::Theme& theme) {
    EditorTheme t;
    visit_editor_theme(t, [&](const char* section, const char* role, glm::vec4& c) { c = theme.color(section, role, c); });
    return t;
}

} // namespace editor
} // namespace toy
