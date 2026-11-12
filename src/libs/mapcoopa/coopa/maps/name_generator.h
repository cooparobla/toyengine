/**
 * @file name_generator.h
 * @brief Invents place names from a per-region synthetic language, so names
 *        within a country sound related and neighbouring countries sound foreign.
 */

#ifndef COOPA_MAPS_NAME_GENERATOR_H
#define COOPA_MAPS_NAME_GENERATOR_H

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <coopa/maps/portable_random.h>

namespace coopa {
namespace maps {

/**
 * @struct Language
 * @brief A synthetic phonology: the pieces place names in one culture are built from.
 *
 * Each country draws its own, so its settlements share a sound. A single global
 * word list would make every nation on every map read in the same voice, which
 * is the thing that gives procedural naming away fastest.
 */
struct Language {
    std::vector<std::string> onsets;   /**< @brief Consonant clusters a syllable may open with. */
    std::vector<std::string> nuclei;   /**< @brief Vowel cores. */
    std::vector<std::string> codas;    /**< @brief Consonant clusters a syllable may close with. */
    std::vector<std::string> prefixes; /**< @brief Optional leading particles, e.g. "Nor". */
    std::vector<std::string> suffixes; /**< @brief Optional trailing particles, e.g. "dale". */

    int min_syllables = 1; /**< @brief Fewest syllables in a generated stem. */
    int max_syllables = 2; /**< @brief Most syllables in a generated stem. */
    /** @brief Chance in `[0, 1]` that a syllable takes a closing consonant. */
    double coda_chance = 0.45;
    /** @brief Chance in `[0, 1]` that a name takes a suffix. */
    double suffix_chance = 0.4;
    /** @brief Chance in `[0, 1]` that a name takes a prefix. */
    double prefix_chance = 0.15;
};

namespace detail {

/** @brief The full inventory a `Language` draws its own subset from. */
inline const std::vector<std::string>& name_onset_pool() {
    static const std::vector<std::string> pool = {
        "b", "br", "d", "dr", "f", "fl", "g", "gr", "h", "k", "kr", "l", "m", "n",
        "p", "pr", "r", "s", "sk", "sl", "st", "t", "th", "tr", "v", "w", "z",
        "kh", "sh", "ch", "gl", "bl", "cl", "vr", "zh", "y", "j", "q"
    };
    return pool;
}

/** @brief Vowel cores available to a language. */
inline const std::vector<std::string>& name_nucleus_pool() {
    static const std::vector<std::string> pool = {
        "a", "e", "i", "o", "u", "ae", "ai", "au", "ea", "ei", "eo", "ia", "ie",
        "io", "oa", "oi", "ou", "ua", "y", "aa", "ee", "oo"
    };
    return pool;
}

/** @brief Closing consonant clusters available to a language. */
inline const std::vector<std::string>& name_coda_pool() {
    static const std::vector<std::string> pool = {
        "n", "r", "l", "s", "m", "th", "sk", "ld", "nd", "rk", "st", "ng", "ch",
        "sh", "rn", "lm", "ft", "x", "z", "k", "t", "d", "g", "v"
    };
    return pool;
}

/** @brief Leading particles a language may attach. */
inline const std::vector<std::string>& name_prefix_pool() {
    static const std::vector<std::string> pool = {
        "Nor", "Sud", "Est", "Vest", "Hoch", "Neu", "Alt", "Far", "Nether", "Upper",
        "Lower", "Grand", "Little", "Old", "New", "High", "Deep", "Fair"
    };
    return pool;
}

/** @brief Trailing particles a language may attach. */
inline const std::vector<std::string>& name_suffix_pool() {
    static const std::vector<std::string> pool = {
        "dale", "ford", "holm", "mere", "wick", "stead", "burg", "heim", "gard",
        "thorpe", "moor", "field", "hollow", "reach", "watch", "crest", "fell",
        "haven", "march", "vale", "barrow", "spire", "keep", "rest"
    };
    return pool;
}

/** @brief Draws `count` distinct entries from `pool`. */
inline std::vector<std::string> draw_subset(const std::vector<std::string>& pool, std::size_t count,
                                            std::mt19937& rng) {
    std::vector<std::string> shuffled = pool;
    coopa::maps::shuffle(shuffled.begin(), shuffled.end(), rng);
    shuffled.resize(std::min(count, shuffled.size()));
    return shuffled;
}

/** @brief Uppercases the first character of a name. */
inline std::string capitalise(std::string text) {
    if (!text.empty()) {
        text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    }
    return text;
}

} // namespace detail

/**
 * @brief Invents a language by drawing a subset of each phoneme pool.
 *
 * The subset is what creates the accent: a language holding only `kh`, `zh` and
 * `vr` as onsets produces a consistently harsh set of names, and one holding
 * `l`, `m` and `w` a soft one.
 *
 * @param rng The caller's seeded generator; the sole source of randomness.
 * @return A language ready to generate names.
 */
inline Language make_language(std::mt19937& rng) {
    coopa::maps::UniformIntDistribution<std::size_t> onset_count(5, 9);
    coopa::maps::UniformIntDistribution<std::size_t> nucleus_count(4, 7);
    coopa::maps::UniformIntDistribution<std::size_t> coda_count(4, 8);
    coopa::maps::UniformIntDistribution<std::size_t> affix_count(3, 6);
    coopa::maps::UniformRealDistribution<double> chance(0.2, 0.45);

    Language language;
    language.onsets = detail::draw_subset(detail::name_onset_pool(), onset_count(rng), rng);
    language.nuclei = detail::draw_subset(detail::name_nucleus_pool(), nucleus_count(rng), rng);
    language.codas = detail::draw_subset(detail::name_coda_pool(), coda_count(rng), rng);
    language.prefixes = detail::draw_subset(detail::name_prefix_pool(), affix_count(rng), rng);
    language.suffixes = detail::draw_subset(detail::name_suffix_pool(), affix_count(rng), rng);
    language.coda_chance = chance(rng);
    language.suffix_chance = chance(rng);
    language.prefix_chance = chance(rng) * 0.3;
    return language;
}

/**
 * @brief Derives a related variant of a language.
 *
 * Keeps most of the parent's inventory and swaps a little, so a region reads as
 * a dialect of its country rather than a foreign tongue.
 *
 * @param parent The country's language.
 * @param rng The caller's seeded generator.
 * @return A dialect of `parent`.
 */
inline Language derive_dialect(const Language& parent, std::mt19937& rng) {
    Language dialect = parent;
    coopa::maps::UniformIntDistribution<std::size_t> pick(0, 2);

    for (std::size_t swaps = pick(rng) + 1; swaps > 0 && !dialect.onsets.empty(); --swaps) {
        coopa::maps::UniformIntDistribution<std::size_t> slot(0, dialect.onsets.size() - 1);
        coopa::maps::UniformIntDistribution<std::size_t> source(0, detail::name_onset_pool().size() - 1);
        dialect.onsets[slot(rng)] = detail::name_onset_pool()[source(rng)];
    }
    if (!dialect.codas.empty()) {
        coopa::maps::UniformIntDistribution<std::size_t> slot(0, dialect.codas.size() - 1);
        coopa::maps::UniformIntDistribution<std::size_t> source(0, detail::name_coda_pool().size() - 1);
        dialect.codas[slot(rng)] = detail::name_coda_pool()[source(rng)];
    }
    return dialect;
}

/**
 * @brief Builds the language belonging to a given seed, reproducibly.
 *
 * A language is generator state, not map data -- serialising phoneme tables
 * into every saved world would bloat it for no gain, since the names are what a
 * consumer needs. Deriving it from an id instead lets any pass rebuild exactly
 * the same language on demand, which is what keeps a region's settlements
 * sounding like they belong to it.
 *
 * @param seed Any stable value, e.g. the map seed mixed with a country id.
 * @return The language for that seed.
 */
inline Language language_for(std::uint32_t seed) {
    std::mt19937 rng(seed);
    return make_language(rng);
}

/**
 * @brief Builds a region's dialect of its country's language, reproducibly.
 * @param country_seed The seed its country's language was built from.
 * @param region_seed A stable value distinguishing this region.
 * @return A dialect of the country's language.
 */
inline Language dialect_for(std::uint32_t country_seed, std::uint32_t region_seed) {
    const Language parent = language_for(country_seed);
    std::mt19937 rng(region_seed);
    return derive_dialect(parent, rng);
}

/** @brief Shortest acceptable place name, in characters. */
inline constexpr std::size_t k_min_name_length = 4;
/** @brief Redraws allowed before a short name is accepted anyway. */
inline constexpr int k_name_length_attempts = 4;

namespace detail {

/**
 * @brief Generates one name without the length guard.
 * @param language The phonology to build from.
 * @param rng The caller's seeded generator.
 * @return A capitalised name.
 */
inline std::string generate_name_once(const Language& language, std::mt19937& rng) {

    const auto pick = [&rng](const std::vector<std::string>& from) -> const std::string& {
        coopa::maps::UniformIntDistribution<std::size_t> index(0, from.size() - 1);
        return from[index(rng)];
    };
    coopa::maps::UniformRealDistribution<double> roll(0.0, 1.0);

    // At most one affix. Allowing both produces names like
    // "Deepwiokdeomdeivmere" -- individually plausible pieces that no one would
    // ever say aloud.
    const bool take_prefix = !language.prefixes.empty() && roll(rng) < language.prefix_chance;
    const bool take_suffix =
        !take_prefix && !language.suffixes.empty() && roll(rng) < language.suffix_chance;

    std::string name;
    if (take_prefix) {
        name += pick(language.prefixes);
    }

    // An affix already carries a syllable, so the stem gives one back.
    const int upper = std::max(language.min_syllables,
                               language.max_syllables - ((take_prefix || take_suffix) ? 1 : 0));
    coopa::maps::UniformIntDistribution<int> syllables(language.min_syllables, upper);
    const int count = syllables(rng);

    for (int i = 0; i < count; ++i) {
        name += pick(language.onsets);
        name += pick(language.nuclei);
        // Never two codas running, and never one right before a suffix: both
        // pile consonants up past the point of pronounceability.
        const bool last = (i == count - 1);
        if (!language.codas.empty() && !(last && take_suffix)
            && roll(rng) < language.coda_chance) {
            name += pick(language.codas);
        }
    }

    if (take_suffix) {
        name += pick(language.suffixes);
    }
    return detail::capitalise(std::move(name));
}

} // namespace detail

/**
 * @brief Generates one place name.
 * @param language The phonology to build from.
 * @param rng The caller's seeded generator.
 * @return A capitalised name, e.g. `"Kherondale"`.
 */
inline std::string generate_name(const Language& language, std::mt19937& rng) {
    if (language.onsets.empty() || language.nuclei.empty()) {
        return "Unnamed";
    }
    // A one-syllable stem with no coda can come out as little as two letters,
    // which reads as a typo rather than a place. Redraw a few times, then take
    // whatever the language gives rather than loop forever on a terse one.
    for (int attempt = 0; attempt < k_name_length_attempts; ++attempt) {
        std::string candidate = detail::generate_name_once(language, rng);
        if (candidate.size() >= k_min_name_length) {
            return candidate;
        }
    }
    return detail::generate_name_once(language, rng);
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_NAME_GENERATOR_H
