/**
 * @file editor_context.h
 * @brief The small, stable surface a panel needs from the editor around it.
 *
 * A panel that needs only a little of the editor takes this instead of the whole `EditorApp`:
 * it keeps the panel's header off `editor_app.h`, and it keeps the panel honest about what it
 * actually depends on. `ConsolePanel` is the example.
 *
 * The surface is deliberately small. Measured over the 25 panels, only ~15 identifiers are used
 * by six or more of them -- the engine, the project, the open document, the theme, the apply /
 * undo entry point and the active asset type. Everything else is either private to one panel
 * (and belongs in that panel's class) or shared by a small cluster (and belongs in a state
 * object that cluster owns).
 *
 * **Grow this type only when a panel genuinely needs something more than one other panel also
 * needs.** Every accessor added here is paid for by every panel, which is exactly the cost the
 * split exists to remove.
 *
 * A panel header forward-declares `EditorContext`; only the panel's `.cpp` includes this file.
 * `EditorApp` holds panels behind forward declarations, so no panel header reaches
 * `editor_app.h`.
 */

#ifndef TOYEDITOR_APP_EDITOR_CONTEXT_H
#define TOYEDITOR_APP_EDITOR_CONTEXT_H

#include <deque>
#include <string>
#include <utility>

namespace toy {
namespace core { class Engine; }
namespace editor {

class EditorApp;

/** @brief A panel's view of the editor. Non-owning; lives as long as its EditorApp. */
class EditorContext {
public:
    explicit EditorContext(EditorApp& app) : app_(app) {}

    EditorContext(const EditorContext&) = delete;
    EditorContext& operator=(const EditorContext&) = delete;

    /// @brief The embedded engine -- the scene, the renderer, input.
    core::Engine& engine();

    /// @brief The console log, newest last: (severity 0..2, message).
    const std::deque<std::pair<int, std::string>>& log() const;
    /// @brief Empties the console.
    void clear_log();

    /// @brief The owning app, for the panels not yet migrated off it. Shrinking: prefer an
    ///        accessor above, and add one here rather than reaching through this.
    EditorApp& app() { return app_; }

private:
    EditorApp& app_;
};

} // namespace editor
} // namespace toy

#endif // TOYEDITOR_APP_EDITOR_CONTEXT_H
