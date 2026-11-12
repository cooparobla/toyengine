/**
 * @file yaml_util.h
 * @brief Small fkYAML helpers the editor uses to read and edit documents in place.
 *
 * Every editor document (scene, mesh, material, config) is a plain fkyaml::node -- the same
 * value the engine's loaders parse -- so these helpers are the editor's whole data-access
 * layer: typed reads with defaults, vector/colour conversion in the engine's own
 * `{ x, y, z }` / `{ r, g, b }` spellings, and key removal.
 */

#ifndef TOYEDITOR_CORE_YAML_UTIL_H
#define TOYEDITOR_CORE_YAML_UTIL_H

#include <fkYAML/node.hpp>
#include <algorithm>
#include <glm/glm.hpp>

#include <string>
#include <vector>

namespace toy::editor {

using Node = fkyaml::node;

/** @brief A float read that accepts YAML ints too (`x: 0` is an integer node). */
float as_float(const Node& n, float def = 0.0f);

float get_float(const Node& map, const std::string& key, float def = 0.0f);
int get_int(const Node& map, const std::string& key, int def = 0);
bool get_bool(const Node& map, const std::string& key, bool def = false);
std::string get_string(const Node& map, const std::string& key, const std::string& def = "");

/** @brief `{ x, y, z }` (or a 3-element sequence) -> vec3. */
glm::vec3 as_vec3(const Node& n, glm::vec3 def = glm::vec3(0.0f));
glm::vec3 get_vec3(const Node& map, const std::string& key, glm::vec3 def = glm::vec3(0.0f));
glm::vec3 get_color(const Node& map, const std::string& key, glm::vec3 def = glm::vec3(1.0f));

/** @brief Rounds away float noise (1e-7 drift from drags) so saved files stay readable. */
double tidy(double v);

inline Node make_float(double v) { return Node(tidy(v)); }
Node make_vec3(const glm::vec3& v);
Node make_color(const glm::vec3& c);
Node make_float_seq(const float* v, int n);

/** @brief Removes `key` from a mapping (no-op if absent or not a mapping). */
void erase_key(Node& map, const std::string& key);

/** @brief The mapping's keys as strings, in fkYAML's (sorted) order. */
std::vector<std::string> keys_of(const Node& map);

/** @brief `map[key]`, creating an empty sequence there if missing (or not a sequence). */
Node& ensure_seq(Node& map, const std::string& key);

/** @brief `map[key]`, creating an empty mapping there if missing (or not a mapping). */
Node& ensure_map(Node& map, const std::string& key);

/**
 * @brief Marks an object node that stands for a child a prefab instance inherits from its
 *        object asset (see SceneDocument::sync_placeholders()). Such a node holds only that
 *        child's overrides, matched to it by name.
 */
inline constexpr const char* kInheritedKey = "__inherited";

/**
 * @brief True for an inherited-child node that overrides nothing: just its name, no
 *        components, and no children besides such placeholders.
 */
bool is_empty_placeholder(const Node& n);

/**
 * @brief Removes every key starting with "__" (editor-private stamps) recursively -- and the
 *        inherited-child placeholders that override nothing, so files only ever hold real
 *        overrides.
 */
void strip_private_keys(Node& n);

/** @brief The component type name of a component node (`type:` key, or a `!Tag`). */
std::string component_type(const Node& comp);

} // namespace toy::editor

#endif // TOYEDITOR_CORE_YAML_UTIL_H
