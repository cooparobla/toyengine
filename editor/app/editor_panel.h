/**
 * @file editor_panel.h
 * @brief Base for the editor's panels: a reference to the app, and nothing else.
 *
 * A panel is one class, in its own header and `.cpp`, owning the state only it uses.
 * `EditorApp` holds panels behind forward declarations (`std::unique_ptr<FooPanel>`), so a
 * panel's header reaches only its own `.cpp`, `editor_app.cpp` and the few sibling panels that
 * call into it -- editing a panel does not recompile the rest.
 *
 * Panels reach the editor through `app()`. Each panel class is a `friend` of `EditorApp`
 * (see the friend block in editor_app.h), so this is a move of *where the state lives*, not a
 * widening of EditorApp's public API: nothing outside the editor gains access.
 *
 * Shared, cheap things (the engine, the document, the theme) are also available through
 * EditorContext; prefer that where it already covers the need.
 */

#ifndef TOYEDITOR_APP_EDITOR_PANEL_H
#define TOYEDITOR_APP_EDITOR_PANEL_H

#include "editor/app/editor_types.h"

namespace toy::editor {

class EditorApp;

/** @brief Common base: holds the owning app by reference. */
class EditorPanel {
public:
    explicit EditorPanel(EditorApp& app) : app_(app) {}

protected:
    EditorApp& app() { return app_; }
    const EditorApp& app() const { return app_; }

    EditorApp& app_;
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_EDITOR_PANEL_H
