/**
 * @file bone_names.h
 * @brief The left/right side of a bone's NAME -- the one string convention that mirroring,
 *        symmetrize and weight flipping all agree on.
 *
 * Blender's BLI_string_flip_side_name, narrowed to the suffixes this project's rigs actually
 * use. A rig pairs its sides by name, not by position (`upper_arm_l` <-> `upper_arm_r`), so this
 * is what every mirror operation keys on; a bone with no side suffix (`spine`, `pelvis`) is a
 * centre bone and has no partner.
 */

#ifndef TOYENGINE_EDITOR_RIG_BONE_NAMES_H
#define TOYENGINE_EDITOR_RIG_BONE_NAMES_H

#include <optional>
#include <string>

namespace toy {
namespace editor {

/**
 * @brief The same bone name on the other side, or nullopt when it carries no side.
 *
 * Handles `_l`/`_r`, `.L`/`.R`, `_left`/`_right` and their case variants, preserving the case of
 * what it replaces so `Hand.L` comes back as `Hand.R` rather than `Hand.r`.
 */
inline std::optional<std::string> mirrored_bone_name(const std::string& name) {
    struct Pair { const char* a; const char* b; };
    // Longest first: "_left" has to be tested before "_l" would match its tail.
    static const Pair kPairs[] = {
        {"_left", "_right"}, {"_Left", "_Right"}, {"_LEFT", "_RIGHT"},
        {".left", ".right"}, {".Left", ".Right"}, {".LEFT", ".RIGHT"},
        {"_l", "_r"}, {"_L", "_R"}, {".l", ".r"}, {".L", ".R"},
    };
    auto ends_with = [&](const std::string& s, const char* suffix) {
        const std::size_t n = std::string(suffix).size();
        return s.size() > n && s.compare(s.size() - n, n, suffix) == 0;
    };
    for (const Pair& p : kPairs) {
        if (ends_with(name, p.a)) return name.substr(0, name.size() - std::string(p.a).size()) + p.b;
        if (ends_with(name, p.b)) return name.substr(0, name.size() - std::string(p.b).size()) + p.a;
    }
    return std::nullopt;
}

} // namespace editor
} // namespace toy

#endif // TOYENGINE_EDITOR_RIG_BONE_NAMES_H
