// Shared sort plumbing: the index vector, stable tie-breaking and the permutation walk that
// moves owning objects, kept out of the header so each comparator does not instantiate its own
// copy.
#include "sort.h"

namespace sort_detail {
void StableSort(void* data, size_t count, void* context, Compare compare, Swap swap) {
    std::vector<size_t> order(count);
    for (size_t i = 0; i < count; ++i) order[i] = i;
    struct Context { void* value; Compare compare; } sorting{context, compare};
    qsort_s(order.data(), count, sizeof(size_t),
        [](void* ctx, const void* left, const void* right) {
            auto& c = *static_cast<Context*>(ctx);
            size_t a = *static_cast<const size_t*>(left), b = *static_cast<const size_t*>(right);
            int result = c.compare(c.value, a, b);
            return result ? result : (a > b) - (a < b);
        }, &sorting);
    // Apply the permutation in place: walk each cycle, swapping each element to where it belongs.
    for (size_t i = 0; i < count; ++i) {
        size_t j = i;
        while (order[j] != i) {
            size_t next = order[j];
            swap(data, j, next);
            order[j] = j;
            j = next;
        }
        order[j] = j;
    }
}
}  // namespace sort_detail
