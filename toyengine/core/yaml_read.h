/**
 * @file yaml_read.h
 * @brief Present-if-set reads from a YAML mapping, for the engine's component parsers.
 *
 * Every parser in this engine reads its keys by hand (there is no reflection -- see
 * editor/schema/component_schema.h's own doc for why), and they all want the same thing:
 * *overwrite this field only if the key is present, otherwise leave the struct's default
 * alone*. Written out that is
 *
 * @code
 * if (node.contains("radius")) cc->radius = node.at("radius").get_value<float>();
 * @endcode
 *
 * which is why a dozen files had each grown their own one-line `f` / `b` / `str` lambda, and
 * `toyengine/scene/register.cpp` alone re-declared the same three fifteen times. Reading a key
 * is one call here:
 *
 * @code
 * using namespace toy::core::yaml;
 * read(node, "radius", cc->radius);
 * read(node, "drag_button", tt->drag_button);
 * read_degrees(node, "wave_direction", w.waves.direction);   // YAML is degrees, the field radians
 * read_rgb(node, "foam_color", w.foam_color);                // {r,g,b} or [r,g,b]
 * read_enum(node, "mode", cc->mode, {{"fly", Mode::Fly}, {"orbit", Mode::Orbit}});
 * @endcode
 *
 * Each returns whether it wrote, for the rare caller that needs to know a key was authored
 * rather than defaulted.
 *
 * **Numbers are int-tolerant.** `x: 0` parses as an integer node, so a plain
 * `get_value<float>()` throws on a perfectly ordinary scene file. `num()` and the float/double
 * overloads of read() accept either; this is the single most common cause of a "this scene
 * won't load" bug and the reason to prefer these over `.at(...).get_value<T>()`.
 */

#ifndef TOYENGINE_CORE_YAML_READ_H
#define TOYENGINE_CORE_YAML_READ_H

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace toy {
namespace core {
namespace yaml {

/** @brief A float from a node that may be an int or a float. 0 if it is neither. */
inline float num(const fkyaml::node& n, float def = 0.0f) {
    if (n.is_float_number()) return static_cast<float>(n.get_value<double>());
    if (n.is_integer()) return static_cast<float>(n.get_value<std::int64_t>());
    return def;
}

/** @brief A double from a node that may be an int or a float. */
inline double num_d(const fkyaml::node& n, double def = 0.0) {
    if (n.is_float_number()) return n.get_value<double>();
    if (n.is_integer()) return static_cast<double>(n.get_value<std::int64_t>());
    return def;
}

/// @name Present-if-set scalar reads
/// Each assigns `node[key]` to `out` when the key exists, and returns whether it did.
/// @{
inline bool read(const fkyaml::node& n, const char* key, float& out) {
    if (!n.is_mapping() || !n.contains(key)) return false;
    out = num(n.at(key), out);
    return true;
}
inline bool read(const fkyaml::node& n, const char* key, double& out) {
    if (!n.is_mapping() || !n.contains(key)) return false;
    out = num_d(n.at(key), out);
    return true;
}
inline bool read(const fkyaml::node& n, const char* key, bool& out) {
    if (!n.is_mapping() || !n.contains(key)) return false;
    out = n.at(key).get_value<bool>();
    return true;
}
inline bool read(const fkyaml::node& n, const char* key, std::string& out) {
    if (!n.is_mapping() || !n.contains(key)) return false;
    out = n.at(key).get_value<std::string>();
    return true;
}
/// Integral fields (int, uint32_t, uint8_t, ...). Narrowed from YAML's int64.
template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
inline bool read(const fkyaml::node& n, const char* key, T& out) {
    if (!n.is_mapping() || !n.contains(key)) return false;
    const fkyaml::node& v = n.at(key);
    out = static_cast<T>(v.is_float_number() ? static_cast<std::int64_t>(v.get_value<double>())
                                             : v.get_value<std::int64_t>());
    return true;
}
/// @}

/** @brief `{x, y}` or a 2-element sequence. */
inline glm::vec2 as_vec2(const fkyaml::node& n, glm::vec2 def = glm::vec2(0.0f)) {
    if (n.is_sequence() && n.size() >= 2) return {num(n[0], def.x), num(n[1], def.y)};
    if (n.is_mapping()) {
        if (n.contains("x")) def.x = num(n.at("x"), def.x);
        if (n.contains("y")) def.y = num(n.at("y"), def.y);
    }
    return def;
}

/** @brief `{x, y, z}` or a 3-element sequence. Components absent keep `def`'s. */
inline glm::vec3 as_vec3(const fkyaml::node& n, glm::vec3 def = glm::vec3(0.0f)) {
    if (n.is_sequence() && n.size() >= 3) return {num(n[0], def.x), num(n[1], def.y), num(n[2], def.z)};
    if (n.is_mapping()) {
        if (n.contains("x")) def.x = num(n.at("x"), def.x);
        if (n.contains("y")) def.y = num(n.at("y"), def.y);
        if (n.contains("z")) def.z = num(n.at("z"), def.z);
    }
    return def;
}

/** @brief `{r, g, b}` or a 3-element sequence. The colour spelling of as_vec3(). */
inline glm::vec3 as_rgb(const fkyaml::node& n, glm::vec3 def = glm::vec3(1.0f)) {
    if (n.is_sequence() && n.size() >= 3) return {num(n[0], def.r), num(n[1], def.g), num(n[2], def.b)};
    if (n.is_mapping()) {
        if (n.contains("r")) def.r = num(n.at("r"), def.r);
        if (n.contains("g")) def.g = num(n.at("g"), def.g);
        if (n.contains("b")) def.b = num(n.at("b"), def.b);
        // A colour written {x, y, z} still reads -- the two spellings are interchangeable.
        if (n.contains("x")) def.r = num(n.at("x"), def.r);
        if (n.contains("y")) def.g = num(n.at("y"), def.g);
        if (n.contains("z")) def.b = num(n.at("z"), def.b);
    }
    return def;
}

inline bool read(const fkyaml::node& n, const char* key, glm::vec2& out) {
    if (!n.is_mapping() || !n.contains(key)) return false;
    out = as_vec2(n.at(key), out);
    return true;
}
inline bool read(const fkyaml::node& n, const char* key, glm::vec3& out) {
    if (!n.is_mapping() || !n.contains(key)) return false;
    out = as_vec3(n.at(key), out);
    return true;
}

/** @brief A `{r,g,b}` / `[r,g,b]` colour, present-if-set. */
inline bool read_rgb(const fkyaml::node& n, const char* key, glm::vec3& out) {
    if (!n.is_mapping() || !n.contains(key)) return false;
    out = as_rgb(n.at(key), out);
    return true;
}

/** @brief A key authored in DEGREES into a field held in radians. */
inline bool read_degrees(const fkyaml::node& n, const char* key, float& out_radians) {
    if (!n.is_mapping() || !n.contains(key)) return false;
    out_radians = glm::radians(num(n.at(key), glm::degrees(out_radians)));
    return true;
}

/**
 * @brief A string key mapped to an enum value. Leaves `out` alone if the key is absent **or**
 *        its value is not one of `options` -- an unknown spelling keeps the default rather
 *        than selecting an arbitrary one.
 */
template <class E>
inline bool read_enum(const fkyaml::node& n, const char* key, E& out,
                      std::initializer_list<std::pair<std::string_view, E>> options) {
    if (!n.is_mapping() || !n.contains(key)) return false;
    const std::string v = n.at(key).get_value<std::string>();
    for (const auto& [name, value] : options) {
        if (v == name) { out = value; return true; }
    }
    return false;
}

} // namespace yaml
} // namespace core
} // namespace toy

#endif // TOYENGINE_CORE_YAML_READ_H
