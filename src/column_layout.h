// The track list's column layout, shared by the Windows list and the Linux port so both size, resize,
// move and sort columns the same way. Portable: no Win32 here, only pixel arithmetic.
//
// Each column keeps its own width, so the list ends where the last column does and the room past it
// stays empty. Dragging a divider resizes the column on its left; the ones after it move along. A
// column never gets narrower than its least width: a column of no width has no divider left to drag it
// back by. The playing indicator is a fixed gutter: it neither resizes nor moves, and nothing lands
// before it. Only the Apollo theme has a stretch column (its "fit the window" Title), which takes the
// room the others leave.
#pragma once

#include <vector>

namespace ui {

// One shown column, in pixels at the current scale.
struct ColumnSpec {
    int type = 0;
    int want = 0;   // saved width, or the column's default
    int least = 0;  // never narrower than this
    bool fixed = false;  // the playing indicator: neither resized nor moved
};

// The least width of a column at 96 DPI. Rating holds five stars; # a four-digit disc and track.
int ColumnLeast(int type);

// Each column's width, left to right. A `stretch` column (an index, or -1 for none) takes what the
// others leave of `room`.
std::vector<int> FitColumns(const std::vector<ColumnSpec>& cols, int room = 0, int stretch = -1);

// Whether the divider on column `k`'s right edge can be dragged.
bool DividerMoves(const std::vector<ColumnSpec>& cols, int k, int stretch = -1);

// The widths after dragging divider `k` by `dx` pixels from `start` (the widths when the drag began):
// column k grows or shrinks, never below its least. Beside a stretch column, the stretch column's own
// divider trades width with the column after it.
std::vector<int> DragDivider(const std::vector<ColumnSpec>& cols, const std::vector<int>& start, int k, int dx,
                             int stretch = -1);

// The draggable divider within `slop` pixels of `x` (measured from the first column's left edge), or -1.
int DividerAt(const std::vector<ColumnSpec>& cols, const std::vector<int>& widths, int x, int slop, int stretch = -1);

// The column under `x`, or -1 past the last one.
int ColumnAtX(const std::vector<int>& widths, int x);

// Where a column dragged to `x` lands: the index it would be inserted before (0..count), never before
// a leading fixed column.
int DropSlot(const std::vector<ColumnSpec>& cols, const std::vector<int>& widths, int x);

// Moves the column at `from` so it lands before `slot` (as DropSlot gives it). False if nothing moved.
template <class T>
bool MoveColumn(std::vector<T>& items, int from, int slot) {
    int n = (int)items.size();
    if (from < 0 || from >= n || slot < 0 || slot > n || slot == from || slot == from + 1) return false;
    T moved = items[from];
    items.erase(items.begin() + from);
    items.insert(items.begin() + (slot > from ? slot - 1 : slot), moved);
    return true;
}

// A header click's sort: ascending, then descending, then (`canClear`) back to the list's own order
// (`column` -1). Columns that are not sort keys (the indicator and #) leave the sort alone.
struct SortState {
    int column = -1;
    bool ascending = true;
};
SortState NextSort(SortState now, int clicked, bool canClear);
constexpr bool SortsBy(int type) { return type > 1; }

}  // namespace ui
