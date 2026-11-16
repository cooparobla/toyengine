#include <toyengine/ui/glass_theme.h>

#include <uicoopa/immediate/imm_theme.h>

namespace toy::ui {

GlassTheme glass_theme_from(const coopa::ui::imm::Theme& theme) {
    GlassTheme t;
    visit_glass_theme(t, [&](const char* section, const char* role, glm::vec4& c) {
        c = theme.color(section, role, c);
    });
    return t;
}

}  // namespace toy::ui
