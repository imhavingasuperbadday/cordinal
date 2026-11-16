// The track list's column layout, shared by both platforms (column_layout.h says how it behaves).
#include "column_layout.h"

#include <algorithm>
#include <cstdlib>

#include "ui_spec.h"

namespace ui {

int ColumnLeast(int type) {
    if (type == 0) return 20;
    if (type == 1) return 32;
    if (type == kColRating) return 80;
    if (type == 2 || type == kColTitleArtist || type == kColApolloTitle) return 80;
    return 40;
}

std::vector<int> FitColumns(const std::vector<ColumnSpec>& cols, int room, int stretch) {
    std::vector<int> widths;
    int used = 0;
    for (int i = 0; i < (int)cols.size(); ++i) {
        const ColumnSpec& c = cols[i];
        widths.push_back(c.fixed ? c.want : std::max(c.want, c.least));
        if (i != stretch) used += widths.back();
    }
    if (stretch >= 0 && stretch < (int)cols.size()) widths[stretch] = std::max(cols[stretch].least, room - used);
    return widths;
}

bool DividerMoves(const std::vector<ColumnSpec>& cols, int k, int stretch) {
    if (k < 0 || k >= (int)cols.size() || cols[k].fixed) return false;
    return k != stretch || (k + 1 < (int)cols.size() && !cols[k + 1].fixed);
}

std::vector<int> DragDivider(const std::vector<ColumnSpec>& cols, const std::vector<int>& start, int k, int dx,
                             int stretch) {
    std::vector<int> widths = start;
    if (!DividerMoves(cols, k, stretch) || start.size() != cols.size()) return widths;
    if (k == stretch) {
        // The stretch column's width is the room left, so its divider gives to or takes from the next one.
        widths[k + 1] = std::max(cols[k + 1].least, start[k + 1] - dx);
        widths[k] = start[k] + start[k + 1] - widths[k + 1];
    } else {
        widths[k] = std::max(cols[k].least, start[k] + dx);
    }
    return widths;
}

int DividerAt(const std::vector<ColumnSpec>& cols, const std::vector<int>& widths, int x, int slop, int stretch) {
    int best = -1, bestDistance = slop + 1, edge = 0;
    for (int k = 0; k < (int)widths.size(); ++k) {
        edge += widths[k];
        int distance = std::abs(x - edge);
        if (distance < bestDistance && DividerMoves(cols, k, stretch)) {
            best = k;
            bestDistance = distance;
        }
    }
    return best;
}

int ColumnAtX(const std::vector<int>& widths, int x) {
    if (x < 0) return -1;
    int left = 0;
    for (int i = 0; i < (int)widths.size(); ++i) {
        if (x < left + widths[i]) return i;
        left += widths[i];
    }
    return -1;
}

int DropSlot(const std::vector<ColumnSpec>& cols, const std::vector<int>& widths, int x) {
    int first = 0;
    while (first < (int)cols.size() && cols[first].fixed) ++first;
    int left = 0, slot = (int)widths.size();
    for (int i = 0; i < (int)widths.size(); ++i) {
        if (x < left + widths[i] / 2) {
            slot = i;
            break;
        }
        left += widths[i];
    }
    return std::max(slot, first);
}

SortState NextSort(SortState now, int clicked, bool canClear) {
    if (!SortsBy(clicked)) return now;
    if (now.column != clicked) return {clicked, true};
    if (now.ascending) return {clicked, false};
    return canClear ? SortState{-1, true} : SortState{clicked, true};
}

}  // namespace ui
