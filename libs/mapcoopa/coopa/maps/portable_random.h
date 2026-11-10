/**
 * @file portable_random.h
 * @brief Standard-library-independent uniform distributions and shuffle, so a
 *        seed produces the same world on every platform.
 *
 * `std::mt19937`'s output sequence is fixed by the standard, but the algorithms
 * that turn it into numbers -- `std::uniform_int_distribution`,
 * `std::uniform_real_distribution` and `std::shuffle` -- are left to the
 * implementation. libstdc++ (Linux) and libc++ (macOS) disagree on all three,
 * so without these the same seed would generate a different map on each.
 *
 * These are drop-in replacements that reproduce libstdc++'s algorithms
 * exactly (GCC 11+: Lemire's nearly-divisionless downscale, the two-draw
 * `generate_canonical`, and the paired-swap `shuffle`), so maps generated on
 * Linux are unchanged and every other platform matches them bit for bit.
 * Verified against map_out.yaml, a Linux-generated `--seed=42` world, together
 * with portable_sort.h and the -ffp-contract=off this repo's CMakeLists.txt
 * applies on macOS.
 *
 * Restricted to 32-bit generators (`std::mt19937`, the only engine mapcoopa
 * uses) and to float/double: those are the only cases implemented, and a
 * static_assert says so rather than silently diverging.
 */

#ifndef COOPA_MAPS_PORTABLE_RANDOM_H
#define COOPA_MAPS_PORTABLE_RANDOM_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>

namespace coopa {
namespace maps {

namespace detail {

/// @brief Requires a generator producing exactly 32 bits, [0, 2^32 - 1].
template <typename G>
constexpr void require_32bit_generator() {
    static_assert(G::min() == 0 && G::max() == 0xFFFFFFFFu,
                  "portable_random: only 32-bit generators (std::mt19937) are supported");
}

/// @brief Lemire's nearly-divisionless downscale of one 32-bit draw to [0, range).
template <typename G>
inline std::uint32_t lemire_downscale(G& g, std::uint32_t range) {
    std::uint64_t product = std::uint64_t(std::uint32_t(g())) * std::uint64_t(range);
    std::uint32_t low = std::uint32_t(product);
    if (low < range) {
        const std::uint32_t threshold = std::uint32_t(0u - range) % range;
        while (low < threshold) {
            product = std::uint64_t(std::uint32_t(g())) * std::uint64_t(range);
            low = std::uint32_t(product);
        }
    }
    return std::uint32_t(product >> 32);
}

/// @brief A double/float in [0, 1) from whole 32-bit draws, as libstdc++'s generate_canonical.
template <typename Real, typename G>
inline Real canonical(G& g) {
    static_assert(std::is_same_v<Real, double> || std::is_same_v<Real, float>,
                  "portable_random: only float and double are supported");
    require_32bit_generator<G>();
    // ceil(digits / 32) draws: 2 for double, 1 for float.
    constexpr int draws = (std::numeric_limits<Real>::digits + 31) / 32;
    Real sum = Real(0);
    Real scale = Real(1);
    for (int k = 0; k < draws; ++k) {
        sum += Real(std::uint32_t(g())) * scale;
        scale *= Real(4294967296.0);
    }
    Real ret = sum / scale;
    if (ret >= Real(1)) ret = std::nextafter(Real(1), Real(0));
    return ret;
}

} // namespace detail

/**
 * @class UniformIntDistribution
 * @brief Drop-in for std::uniform_int_distribution: uniform integer in [a, b].
 */
template <typename IntType = int>
class UniformIntDistribution {
public:
    using result_type = IntType;

    explicit UniformIntDistribution(IntType a = 0,
                                    IntType b = std::numeric_limits<IntType>::max())
        : a_(a), b_(b) {}

    template <typename G>
    result_type operator()(G& g) const { return draw_(g, a_, b_); }

    result_type a() const { return a_; }
    result_type b() const { return b_; }

private:
    template <typename G>
    static result_type draw_(G& g, IntType a, IntType b) {
        detail::require_32bit_generator<G>();
        using U  = std::make_unsigned_t<IntType>;
        using UC = std::common_type_t<typename G::result_type, U>;
        constexpr UC urngrange = UC(0xFFFFFFFFu);
        const UC urange = UC(b) - UC(a);

        UC ret;
        if (urngrange > urange) {
            ret = UC(detail::lemire_downscale(g, std::uint32_t(urange + 1)));
        } else if (urngrange < urange) {
            // Upscaling: compose a high part and one full draw, rejecting overflow.
            // Only reachable when UC is wider than 32 bits.
            if constexpr (sizeof(UC) > sizeof(std::uint32_t)) {
                UC tmp;
                do {
                    const UC uerngrange = urngrange + 1;
                    tmp = uerngrange * UC(draw_(g, IntType(0), IntType(urange / uerngrange)));
                    ret = tmp + UC(std::uint32_t(g()));
                } while (ret > urange || ret < tmp);
            } else {
                ret = UC(std::uint32_t(g()));
            }
        } else {
            ret = UC(std::uint32_t(g()));
        }
        return result_type(ret + UC(a));
    }

    IntType a_;
    IntType b_;
};

/**
 * @class UniformRealDistribution
 * @brief Drop-in for std::uniform_real_distribution: uniform real in [a, b).
 */
template <typename RealType = double>
class UniformRealDistribution {
public:
    using result_type = RealType;

    explicit UniformRealDistribution(RealType a = RealType(0), RealType b = RealType(1))
        : a_(a), b_(b) {}

    template <typename G>
    result_type operator()(G& g) const {
        // No FMA: libstdc++ rounds the product and the sum separately, and so must we.
#ifdef __clang__
#pragma clang fp contract(off)
#endif
        const RealType u = detail::canonical<RealType>(g);
        const RealType scaled = u * (b_ - a_);
        return scaled + a_;
    }

    result_type a() const { return a_; }
    result_type b() const { return b_; }

private:
    RealType a_;
    RealType b_;
};

/**
 * @brief Drop-in for std::shuffle, matching libstdc++'s sequence exactly --
 *        including its two-swaps-per-draw fast path for short ranges.
 */
template <typename RandomIt, typename G>
inline void shuffle(RandomIt first, RandomIt last, G&& g) {
    if (first == last) return;

    using Gen = std::remove_reference_t<G>;
    detail::require_32bit_generator<Gen>();
    using Diff = typename std::iterator_traits<RandomIt>::difference_type;
    using UD   = std::make_unsigned_t<Diff>;
    using UC   = std::common_type_t<typename Gen::result_type, UD>;

    const UC urngrange = UC(Gen::max() - Gen::min());
    const UC urange    = UC(last - first);

    if (urngrange / urange >= urange) {
        RandomIt i = first + 1;
        if ((urange % 2) == 0) {
            UniformIntDistribution<UD> d(0, 1);
            std::iter_swap(i++, first + d(g));
        }
        while (i != last) {
            const UC swap_range = UC(i - first) + 1;
            const UC x = UniformIntDistribution<UC>(0, swap_range * (swap_range + 1) - 1)(g);
            std::iter_swap(i++, first + x / (swap_range + 1));
            std::iter_swap(i++, first + x % (swap_range + 1));
        }
        return;
    }

    for (RandomIt i = first + 1; i != last; ++i) {
        std::iter_swap(i, first + UniformIntDistribution<UD>(0, UD(i - first))(g));
    }
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PORTABLE_RANDOM_H
