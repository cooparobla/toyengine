/**
 * @file particle_panel.h
 * @brief The ParticleSystem inspector (Properties > Components). A particle system has ~90
 *        settings, most of which only mean something for one mode, shape or render mode -- so
 *        instead of the flat schema list, this panel: - leads with what kind of system it is
 *        (Emitter: born over time / Scatter: placed once), a live status line, and preview
 *        controls (Restart / Pause) for the editor's running preview; - groups the settings into
 *        sections that each say, in a sentence, what they control; - shows only the settings that
 *        apply (a cone's angle only for a cone, lifetime only for an emitter, the sprite look
 *        only for sprites, the terrain block only for `shape: terrain`); - gives terrain scatter
 *        its own section: terrain and paint layer pickers, density with read-outs (spacing,
 *        instances per chunk) and quick halve / double, slope and height bands, draw distance,
 *        and the live chunk / instance counts.
 *
 * Every field is still the schema's FieldDesc (same widgets, undo, prefab-override marks); the
 * panel only decides which appear, where, and with what words.
 *
 * One class per panel, owning its own state (see editor/app/README.md). EditorApp holds it
 * behind a forward declaration, so editing this header recompiles this panel and
 * editor_app.cpp -- not every panel.
 */

#ifndef TOYEDITOR_APP_UI_PARTICLE_PANEL_H
#define TOYEDITOR_APP_UI_PARTICLE_PANEL_H

#include "editor/app/editor_panel.h"

namespace toy::editor {

/** @brief See the file doc. */
class ParticlePanel : public EditorPanel {
public:
    using EditorPanel::EditorPanel;

    /// EditorApp owns this panel and drives it; the declarations below keep the access level
    /// they had inside EditorApp's body.
    friend class EditorApp;

    /** @brief Draws the ParticleSystem component `comp` of object `id`. Edits land in `comp`. */
    EditResult draw_particle_panel_(imm::Context& ctx, ObjectId id, Node& comp, const InspectorEnv& env);

    /** @brief The live ParticleSystem of `id` (null when not built). */
    toy::particles::ParticleSystem* live_particle_system_(ObjectId id) const;

    /** @brief The terrain section of a `shape: terrain` scatter. */
    EditResult draw_particle_terrain_section_(imm::Context& ctx, ObjectId id, Node& comp, const InspectorEnv& env);
};

} // namespace toy::editor

#endif // TOYEDITOR_APP_UI_PARTICLE_PANEL_H
