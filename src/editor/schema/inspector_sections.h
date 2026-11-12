/**
 * @file inspector_sections.h
 * @brief The registry of hand-written inspector sections: what a component reaches for when
 *        rows of fields are not enough.
 *
 * A schema's InspectorSection can name a `custom` drawer, and a whole component can be taken
 * over by registering a drawer under its type name. A drawer is an ordinary function of
 * (imm::Context&, SectionContext&) -- it gets the component's YAML node, the InspectorEnv it
 * needs to reach the editor around it, and reports what it changed the same way every other
 * row does, so undo, merging and prefab overrides keep working.
 *
 * Built-in drawers live in editor/schema/sections/ and are registered by
 * register_builtin_sections(). A game project registers its own the same way it registers a
 * schema, under `#if TOY_EDITOR`.
 */

#ifndef TOYEDITOR_SCHEMA_INSPECTOR_SECTIONS_H
#define TOYEDITOR_SCHEMA_INSPECTOR_SECTIONS_H

#include "inspector.h"

#include <functional>
#include <set>
#include <string>

namespace toy::editor {

/** @brief What a custom drawer is handed. */
struct SectionContext {
    Node& comp;                      ///< The component's block; edits go here, in place.
    const InspectorEnv& env;
    ObjectId object_id = 0;          ///< The object being inspected (0 outside a scene).
    std::set<std::string>* drawn = nullptr;   ///< Keys the drawer handled: the section skips them.

    /** @brief Marks `key` as drawn, so the section does not draw it again. */
    void handled(const std::string& key) const { if (drawn) drawn->insert(key); }
};

/** @brief Draws a block of inspector UI for one component; see the file doc. */
using SectionDrawFn = std::function<EditResult(imm::Context&, SectionContext&)>;

/**
 * @brief Registers (or replaces) a drawer.
 * @param id Either a component type name -- which takes over that component's whole inspector
 *           -- or `"<Type>.<section>"` for one section's `custom` id.
 * @return true, so it can initialise a static.
 */
bool register_inspector_section(std::string id, SectionDrawFn fn);

/** @brief The drawer registered under `id`, or null. */
const SectionDrawFn* find_inspector_section(const std::string& id);

/**
 * @brief Forgets the drawer registered under `id`.
 *
 * A drawer that closes over something with a shorter life than the registry -- the editor
 * application itself, which tests build and tear down repeatedly -- must drop itself when
 * that thing goes, or the next lookup hands back a function over freed memory.
 */
void unregister_inspector_section(const std::string& id);

/** @brief Registers every drawer in editor/schema/sections/. Called once, at editor start-up. */
void register_builtin_sections();

} // namespace toy::editor

#endif // TOYEDITOR_SCHEMA_INSPECTOR_SECTIONS_H
