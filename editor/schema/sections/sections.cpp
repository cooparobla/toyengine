/**
 * @file sections.cpp
 * @brief Registers every built-in hand-written inspector drawer.
 *
 * One file per component beside this one; each exposes a `register_<component>_section()`
 * that this calls. Keeping the list here, rather than in static initialisers, means the
 * order is explicit and a drawer can be found by grepping one file.
 */

#include "editor/schema/inspector_sections.h"

namespace toy {
namespace editor {

void register_procgraph_section();

void register_builtin_sections() {
    static bool done = false;
    if (done) return;
    done = true;
    register_procgraph_section();
}

} // namespace editor
} // namespace toy
