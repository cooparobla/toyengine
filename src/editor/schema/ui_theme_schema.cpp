#include "editor/schema/ui_theme_schema.h"

#include <uicoopa/immediate/imm.h>
#include <uicoopa/immediate/imm_theme.h>

#include <cctype>

#include <toyengine/ui/game_theme.h>

namespace toy {
namespace editor {

const std::vector<std::string>& ui_theme_font_roles() {
    // One face per theme now. The retained themes had six typographic roles; an immediate
    // screen asks for a size per call instead, so there is nothing to give a role to.
    static const std::vector<std::string> r = {};
    return r;
}

const std::vector<ThemeSection>& ui_theme_sections() {
    static const std::vector<ThemeSection> sections = [] {
        std::vector<ThemeSection> s;

        // Metrics and colours come straight from visit_style() -- the same list the loader and
        // the serializer walk, so the editor cannot offer a field the format does not have, or
        // miss one it gained.
        struct StyleVisitor {
            std::vector<FieldDesc> metrics;
            std::vector<FieldDesc> colors;
            void metric(const char* key, float v) {
                metrics.push_back(f_float(key, v, 0.1f, 0.0f, 400.0f));
            }
            void color(const char* key, const glm::vec4& v) { colors.push_back(f_color4(key, v)); }
        };
        StyleVisitor sv;
        coopa::ui::imm::Style style;
        coopa::ui::imm::visit_style(style, sv);
        s.push_back({"metrics", "Metrics", std::move(sv.metrics)});
        s.push_back({"colors", "Widgets", std::move(sv.colors)});

        // And the game roles, from their own list (toyengine/ui/game_theme.h).
        struct GameVisitor {
            std::vector<ThemeSection>* out;
            void operator()(const char* section, const char* role, glm::vec4& v) {
                if (out->empty() || out->back().key != section) {
                    std::string title = section;
                    if (!title.empty()) title[0] = static_cast<char>(std::toupper(title[0]));
                    out->push_back({section, title, {}});
                }
                out->back().fields.push_back(f_color4(role, v));
            }
        };
        toy::ui::GameTheme game;
        std::vector<ThemeSection> game_sections;
        GameVisitor gv{&game_sections};
        toy::ui::visit_game_theme(game, gv);
        for (ThemeSection& g : game_sections) s.push_back(std::move(g));
        return s;
    }();
    return sections;
}

} // namespace editor
} // namespace toy
