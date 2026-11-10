/**
 * @file portable_sort.h
 * @brief A standard-library-independent `sort`, so a seed produces the same
 *        world on every platform.
 *
 * `std::sort` is not stable, and the order it leaves *equal* elements in is up
 * to the implementation: libstdc++ (Linux) and libc++ (macOS) disagree. Several
 * passes sort with comparators that can tie (moisture ranks, town scores,
 * distances), and any tie ordered differently changes the map.
 *
 * coopa::maps::sort is a faithful port of libstdc++'s introsort (median-of-three
 * pivot, unguarded partition, heap-sort fallback at 2*lg(n) depth, final
 * insertion sort with a 16-element threshold), so maps generated on Linux are
 * unchanged and every other platform matches them exactly. See
 * portable_random.h for the matching distributions.
 */

#ifndef COOPA_MAPS_PORTABLE_SORT_H
#define COOPA_MAPS_PORTABLE_SORT_H

#include <algorithm>
#include <bit>
#include <iterator>
#include <type_traits>
#include <utility>

namespace coopa {
namespace maps {

namespace detail {
namespace sort_impl {

constexpr int k_threshold = 16;

template <typename It, typename Diff, typename T, typename Comp>
void push_heap(It first, Diff hole, Diff top, T value, Comp& comp) {
    Diff parent = (hole - 1) / 2;
    while (hole > top && comp(*(first + parent), value)) {
        *(first + hole) = std::move(*(first + parent));
        hole = parent;
        parent = (hole - 1) / 2;
    }
    *(first + hole) = std::move(value);
}

template <typename It, typename Diff, typename T, typename Comp>
void adjust_heap(It first, Diff hole, Diff len, T value, Comp& comp) {
    const Diff top = hole;
    Diff second = hole;
    while (second < (len - 1) / 2) {
        second = 2 * (second + 1);
        if (comp(*(first + second), *(first + (second - 1)))) {
            --second;
        }
        *(first + hole) = std::move(*(first + second));
        hole = second;
    }
    if ((len & 1) == 0 && second == (len - 2) / 2) {
        second = 2 * (second + 1);
        *(first + hole) = std::move(*(first + (second - 1)));
        hole = second - 1;
    }
    sort_impl::push_heap(first, hole, top, std::move(value), comp);
}

template <typename It, typename Comp>
void pop_heap(It first, It last, It result, Comp& comp) {
    using Diff = typename std::iterator_traits<It>::difference_type;
    auto value = std::move(*result);
    *result = std::move(*first);
    sort_impl::adjust_heap(first, Diff(0), Diff(last - first), std::move(value), comp);
}

template <typename It, typename Comp>
void make_heap(It first, It last, Comp& comp) {
    using Diff = typename std::iterator_traits<It>::difference_type;
    if (last - first < 2) return;
    const Diff len = last - first;
    Diff parent = (len - 2) / 2;
    while (true) {
        auto value = std::move(*(first + parent));
        sort_impl::adjust_heap(first, parent, len, std::move(value), comp);
        if (parent == 0) return;
        --parent;
    }
}

template <typename It, typename Comp>
void heap_sort_range(It first, It last, Comp& comp) {
    sort_impl::make_heap(first, last, comp);  // heap_select with middle == last
    while (last - first > 1) {
        --last;
        sort_impl::pop_heap(first, last, last, comp);
    }
}

template <typename It, typename Comp>
void move_median_to_first(It result, It a, It b, It c, Comp& comp) {
    if (comp(*a, *b)) {
        if (comp(*b, *c))      std::iter_swap(result, b);
        else if (comp(*a, *c)) std::iter_swap(result, c);
        else                   std::iter_swap(result, a);
    } else if (comp(*a, *c))   std::iter_swap(result, a);
    else if (comp(*b, *c))     std::iter_swap(result, c);
    else                       std::iter_swap(result, b);
}

template <typename It, typename Comp>
It unguarded_partition(It first, It last, It pivot, Comp& comp) {
    while (true) {
        while (comp(*first, *pivot)) ++first;
        --last;
        while (comp(*pivot, *last)) --last;
        if (!(first < last)) return first;
        std::iter_swap(first, last);
        ++first;
    }
}

template <typename It, typename Comp>
It unguarded_partition_pivot(It first, It last, Comp& comp) {
    It mid = first + (last - first) / 2;
    sort_impl::move_median_to_first(first, first + 1, mid, last - 1, comp);
    return sort_impl::unguarded_partition(first + 1, last, first, comp);
}

template <typename It, typename Diff, typename Comp>
void introsort_loop(It first, It last, Diff depth_limit, Comp& comp) {
    while (last - first > k_threshold) {
        if (depth_limit == 0) {
            sort_impl::heap_sort_range(first, last, comp);
            return;
        }
        --depth_limit;
        It cut = sort_impl::unguarded_partition_pivot(first, last, comp);
        sort_impl::introsort_loop(cut, last, depth_limit, comp);
        last = cut;
    }
}

template <typename It, typename Comp>
void unguarded_linear_insert(It last, Comp& comp) {
    auto value = std::move(*last);
    It next = last;
    --next;
    while (comp(value, *next)) {
        *last = std::move(*next);
        last = next;
        --next;
    }
    *last = std::move(value);
}

template <typename It, typename Comp>
void insertion_sort(It first, It last, Comp& comp) {
    if (first == last) return;
    for (It i = first + 1; i != last; ++i) {
        if (comp(*i, *first)) {
            auto value = std::move(*i);
            std::move_backward(first, i, i + 1);
            *first = std::move(value);
        } else {
            sort_impl::unguarded_linear_insert(i, comp);
        }
    }
}

template <typename It, typename Comp>
void final_insertion_sort(It first, It last, Comp& comp) {
    if (last - first > k_threshold) {
        sort_impl::insertion_sort(first, first + k_threshold, comp);
        for (It i = first + k_threshold; i != last; ++i) {
            sort_impl::unguarded_linear_insert(i, comp);
        }
    } else {
        sort_impl::insertion_sort(first, last, comp);
    }
}

} // namespace sort_impl
} // namespace detail

/**
 * @brief Drop-in for std::sort(first, last, comp) with libstdc++'s exact
 *        element order, ties included, on every platform.
 */
template <typename RandomIt, typename Compare>
inline void sort(RandomIt first, RandomIt last, Compare comp) {
    if (first == last) return;
    using Diff = typename std::iterator_traits<RandomIt>::difference_type;
    const Diff n = last - first;
    // std::__lg: floor(log2(n)).
    const Diff lg = Diff(std::bit_width(static_cast<std::make_unsigned_t<Diff>>(n))) - 1;
    detail::sort_impl::introsort_loop(first, last, lg * 2, comp);
    detail::sort_impl::final_insertion_sort(first, last, comp);
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PORTABLE_SORT_H
