// One shared sort path instead of an instantiation per record type and comparator: SortRecords
// for trivially copyable values (raw qsort_s relocation) and SortObjects for owning values
// (sorted through an index permutation).
#pragma once
#include <cstdlib>
#include <functional>
#include <iterator>
#include <memory>
#include <type_traits>
#include <vector>

#ifndef _WIN32
// The Microsoft qsort_s (context first) on glibc's qsort_r (context last).
inline void qsort_s(void* base, size_t count, size_t size, int (*compare)(void*, const void*, const void*), void* context) {
    struct Call {
        int (*compare)(void*, const void*, const void*);
        void* context;
    } call{compare, context};
    qsort_r(base, count, size, [](const void* a, const void* b, void* c) {
        auto* call = static_cast<Call*>(c);
        return call->compare(call->context, a, b);
    }, &call);
}
#endif

// The type-erased half of SortObjects (defined in sort.cpp): sorts indices with the CRT and
// applies the permutation through `swap`, keeping equal keys in their original order.
namespace sort_detail {
using Compare = int (*)(void*, size_t, size_t);
using Swap = void (*)(void*, size_t, size_t);
void StableSort(void* data, size_t count, void* context, Compare compare, Swap swap);
}  // namespace sort_detail

// Raw relocation is valid only for trivially copyable records. Strings and owning objects
// use ordinary C++ moves. Each call owns its comparator context (also when nested).
template <class Iterator, class Less = std::less<>>
void SortRecords(Iterator first, Iterator last, Less less = {}) {
    using T = typename std::iterator_traits<Iterator>::value_type;
    static_assert(std::contiguous_iterator<Iterator> && std::is_trivially_copyable_v<T>);
    if (first == last) return;
    qsort_s(std::to_address(first), (size_t)(last - first), sizeof(T),
        [](void* context, const void* left, const void* right) {
            auto& compare = *static_cast<Less*>(context);
            const auto& a = *static_cast<const T*>(left);
            const auto& b = *static_cast<const T*>(right);
            return compare(a, b) ? -1 : (int)compare(b, a);
        }, &less);
}

// Sort owning objects through indices and apply the permutation using ordinary C++ moves.
// Only the indices are byte-relocated. Original indices also make equal keys stable.
template <class Iterator, class Less = std::less<>>
void SortObjects(Iterator first, Iterator last, Less less = {}) {
    using T = typename std::iterator_traits<Iterator>::value_type;
    static_assert(std::contiguous_iterator<Iterator>);
    if (first == last) return;
    struct Context { Iterator first; Less* less; } context{first, &less};
    sort_detail::StableSort(std::to_address(first), (size_t)(last - first), &context,
        [](void* ctx, size_t a, size_t b) {
            auto& c = *static_cast<Context*>(ctx);
            if ((*c.less)(c.first[a], c.first[b])) return -1;
            return (int)(*c.less)(c.first[b], c.first[a]);
        },
        [](void* data, size_t a, size_t b) {
            using std::swap;
            auto* values = static_cast<T*>(data);
            swap(values[a], values[b]);
        });
}
