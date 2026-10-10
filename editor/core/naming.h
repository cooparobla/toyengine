/**
 * @file naming.h
 * @brief The snake_case names the editor gives what it creates -- scenes, objects, asset files --
 *        matching assets/ ("camera", "water_plane", "wood_crate.yaml").
 */

#ifndef TOYEDITOR_CORE_NAMING_H
#define TOYEDITOR_CORE_NAMING_H

#include <cctype>
#include <string>

namespace toy::editor {

/**
 * @brief "Point Light" -> "point_light", "ReflectionProbe" -> "reflection_probe",
 *        "my-Asset 2" -> "my_asset_2". Lower-case letters and digits joined by single
 *        underscores; a word boundary is a space / punctuation or a lower-to-upper change.
 */
std::string snake_case(const std::string& s);

} // namespace toy::editor

#endif // TOYEDITOR_CORE_NAMING_H
