/**
 * @file volcano.h
 * @brief What sort of volcano a vent is, and the shape that makes it read as one.
 */

#ifndef COOPA_MAPS_VOLCANO_H
#define COOPA_MAPS_VOLCANO_H

#include <cstddef>
#include <string_view>

namespace coopa {
namespace maps {

/**
 * @enum VolcanoKind
 * @brief The sort of edifice a vent builds.
 *
 * Not decoration, and not a palette: these differ in the *shape* of the ground, and no amount
 * of tuning one curve produces the others. A stratovolcano's flanks are concave and its crater
 * is a nick in the summit; a shield's are nearly straight and span several times the width at a
 * fraction of the height; a collapse caldera is mostly a flat floor inside a raised wall, with
 * the crater over half the edifice's own radius; and a fissure is not a cone at all but a line.
 *
 * `Mixed` is the default and hashes a kind per vent, so a world gets a population rather than
 * one shape repeated. Naming a single kind is what the demo scenes and a themed world do.
 *
 * Names are append-only, exactly as `Biome`'s and `CaveZone`'s are: a kind's position is not
 * its identity, and inserting above would silently reinterpret every vent in every saved map.
 */
enum class VolcanoKind {
    Mixed,          /**< @brief Choose per vent. The default. */
    Shield,         /**< @brief Broad and low, nearly straight flanks -- Hawaii, Olympus Mons. */
    Stratovolcano,  /**< @brief The steep composite cone, concave flanks, small summit crater. */
    Caldera,        /**< @brief A wide collapse basin inside a raised rim. */
    Fissure,        /**< @brief A rift: vents along a line rather than at a point. */
    CinderCone,     /**< @brief Small, steep, with a crater wide for its size. */
    LavaDome        /**< @brief Small, bulbous and crater-less: viscous lava piled over its vent. */
};

/** @brief Number of distinct `VolcanoKind` values; the size of any kind-indexed table. */
inline constexpr std::size_t k_volcano_kind_count = 7;

/** @brief Maps a kind to its serialisation name. */
inline std::string_view volcano_kind_name(VolcanoKind kind) {
    switch (kind) {
        case VolcanoKind::Mixed:         return "mixed";
        case VolcanoKind::Shield:        return "shield";
        case VolcanoKind::Stratovolcano: return "stratovolcano";
        case VolcanoKind::Caldera:       return "caldera";
        case VolcanoKind::Fissure:       return "fissure";
        case VolcanoKind::CinderCone:    return "cinder_cone";
        case VolcanoKind::LavaDome:      return "lava_dome";
    }
    return "mixed";
}

/** @brief Resolves a serialisation name back to a kind; unknown text reads as `Mixed`. */
inline VolcanoKind volcano_kind_from_name(std::string_view name) {
    for (std::size_t i = 0; i < k_volcano_kind_count; ++i) {
        const VolcanoKind candidate = static_cast<VolcanoKind>(i);
        if (volcano_kind_name(candidate) == name) {
            return candidate;
        }
    }
    return VolcanoKind::Mixed;
}

/**
 * @struct VolcanoShape
 * @brief The numbers that turn one radial profile into six recognisable landforms.
 *
 * All of them are RELATIVE -- to the lattice's own vent radius, or to the cone's peak -- so the
 * table says nothing about world scale and a kind looks like itself at any `volcano_size`.
 */
struct VolcanoShape {
    /** @brief Multiplies the lattice's vent radius. A shield spans several times a strato cone. */
    double radius = 1.0;
    /** @brief Multiplies the peak rise. Trades against `radius` to set how steep the thing is. */
    double height = 1.0;
    /**
     * @brief Exponent of the flank falloff, `pow(1 - u, flank)`.
     *
     * Below 1 the top is flat and the edges steep (a dome); 1 is a straight cone; above 1 the
     * flanks go concave and the summit sharpens, which is what a composite cone looks like.
     */
    double flank = 1.0;
    /** @brief Crater radius as a fraction of the edifice's own; 0 for a kind with no crater. */
    double crater = 0.0;
    /** @brief How far the crater floor drops, as a fraction of the peak, at full `volcano_caldera`. */
    double floor = 0.0;
    /** @brief Height of the ring standing at the crater's edge, as a fraction of the peak. */
    double rim = 0.0;
    /** @brief Half-length of the vent LINE, as a fraction of the radius; 0 makes it a point. */
    double length = 0.0;
    /**
     * @brief How full of molten lava the crater stands, as a fraction of floor-to-rim depth.
     *
     * 0 is a cold vent, and it is the honest answer for three of the six: a cinder cone is a
     * pile of ash, a lava dome IS its own solidified lava, and a fissure has no basin to pool
     * in -- what stands over a rift is a fountain, which is an effect rather than a surface.
     * A kind with no crater reads 0 whatever this says, since there is no depth to fill.
     */
    double lava = 0.0;
};

/**
 * @brief The shape of one kind.
 *
 * `Mixed` never reaches here -- it is resolved to a real kind per vent before the shape is
 * asked for -- but it answers with the stratovolcano so a caller that forgets gets a volcano
 * rather than a flat spot.
 */
inline VolcanoShape volcano_shape(VolcanoKind kind) {
    switch (kind) {
        //                       radius height flank crater floor  rim  length lava
        case VolcanoKind::Shield:        return {2.20, 0.45, 1.00, 0.10, 0.25, 0.05, 0.0, 0.75};
        case VolcanoKind::Stratovolcano: return {1.00, 1.30, 2.60, 0.12, 0.45, 0.08, 0.0, 0.45};
        case VolcanoKind::Caldera:       return {1.60, 0.80, 1.80, 0.55, 1.00, 0.45, 0.0, 0.35};
        case VolcanoKind::Fissure:       return {0.55, 0.50, 1.60, 0.00, 0.00, 0.00, 2.6, 0.00};
        case VolcanoKind::CinderCone:    return {0.35, 0.50, 1.40, 0.35, 0.60, 0.18, 0.0, 0.00};
        case VolcanoKind::LavaDome:      return {0.30, 0.55, 0.70, 0.00, 0.00, 0.00, 0.0, 0.00};
        case VolcanoKind::Mixed:         break;
    }
    return {1.00, 1.30, 2.60, 0.12, 0.45, 0.08, 0.0, 0.45};
}

/**
 * @brief The largest `VolcanoShape::height` any kind uses.
 *
 * What the surface has to reserve room for before it adds a cone: one vent of the tallest kind
 * at full size. Deliberately NOT the sum over octaves -- two vents of different octaves landing
 * exactly on top of each other at full scale is possible and vanishingly rare, and reserving for
 * it flattens every world that never sees it.
 */
inline constexpr double k_volcano_tallest = 1.3;   // Stratovolcano's `height`

/** @brief The kinds `Mixed` draws from, in the order a per-vent hash indexes them. */
inline constexpr std::size_t k_volcano_mixed_count = 6;

/** @brief The i'th real kind, for resolving `Mixed`. */
inline VolcanoKind volcano_mixed_kind(std::size_t i) {
    // Weighted by repetition rather than by a float table: cinder cones and domes are the
    // commonest things on a volcanic landscape and calderas the rarest, and an integer hash
    // over this list says so without a cumulative-probability walk.
    static constexpr VolcanoKind k_pool[] = {
        VolcanoKind::CinderCone, VolcanoKind::Stratovolcano, VolcanoKind::LavaDome,
        VolcanoKind::Shield,     VolcanoKind::Fissure,       VolcanoKind::Caldera,
    };
    return k_pool[i % k_volcano_mixed_count];
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_VOLCANO_H
