// Purpose-built immediate-mode widgets (UI_ namespace) for this screen's
// tab strip and directory row-lists - not a generic retained/hashed
// widget-tree framework (raddebugger's UI_ box-tree approach), which isn't
// warranted yet at this scale. Callers own their own small persistent
// state (selected tab index, UI_RowListState per row-list) explicitly,
// rather than this layer hashing/keying widget identity itself.

#ifndef UI_CORE_H
#define UI_CORE_H

typedef struct UI_RowListState UI_RowListState;
struct UI_RowListState
{
  S32 last_click_index;
  U64 last_click_time_us;
  F32 scroll_y; // pixels scrolled down from the top - caller-owned, persists across frames
};

// Call ui_begin_frame once per frame before any other ui_* calls, and
// ui_end_frame once after - this caches the current mouse state and
// commits it as "previous frame" for next frame's press-edge detection,
// so multiple widgets queried within the same frame agree on whether a
// click just happened (rather than each re-deriving edge state and
// disagreeing after the first one consumes the transition).
internal void ui_begin_frame(WM_Window window);
internal void ui_end_frame(void);

// dr_text's y is the text BASELINE, not the top of the glyph box (glyphs
// extend upward from it) - this approximates a good baseline Y to
// vertically center `size`-px text within a box spanning [box_top,
// box_top+box_h), using ascent ~= 0.7*size (a reasonable stand-in for
// real font metrics, which fp_dwrite doesn't expose - see fp_dwrite.h).
// Getting this wrong looks like text rendering "too high", overlapping
// whatever's above the box.
internal F32 ui_text_baseline_y(F32 box_top, F32 box_h, F32 size);

// Draws `label_count` equal-width tabs left-to-right from (x,y). Returns
// the index clicked this frame (a fresh press, not held), or -1.
internal S32 ui_tab_strip(FP_Font *font, F32 x, F32 y, F32 tab_w, F32 tab_h,
                           String8 *labels, U32 label_count, S32 selected);

// Draws `entries` as icon + name (+ size, for files) rows in the pane
// rect (x,y,x+width,y+height), each `row_h` tall, hover-highlighted under
// the mouse, clipped to that rect and mouse-wheel scrollable when there
// are more rows than fit (scroll position lives in `state`, so it's
// per-pane and persists frame to frame). Returns the row index clicked
// this frame (-1 if none); *out_double_clicked reports whether that click
// was a double-click on the same row as last time *for this `state`
// instance* - callers own one UI_RowListState per row-list on screen, so
// clicking in one pane can't be mistaken for continuing a double-click in
// another.
internal S32 ui_row_list(Arena *frame_arena, FP_Font *font, F32 x, F32 y, F32 width, F32 height, F32 row_h,
                          FS_Entry *entries, U64 entry_count,
                          UI_RowListState *state, B32 *out_double_clicked);

#endif // UI_CORE_H
