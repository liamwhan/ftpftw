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

// A single-line text field's persistent state - one per field, caller-
// owned, same model as UI_RowListState. Fixed-capacity buffer, no
// malloc/realloc - 256 bytes is generous for a host/username/password.
typedef struct UI_TextEditState UI_TextEditState;
struct UI_TextEditState
{
  U8 buffer[256];
  U64 len;
  U64 cursor;
};

// Hit-testing/geometry helper, shared beyond the two widgets below (e.g.
// the Connections modal's own panel/buttons).
internal B32 ui__point_in_rect(F32 px, F32 py, F32 x0, F32 y0, F32 x1, F32 y1);

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

// --- text edit state helpers -------------------------------------------------

internal void    ui_text_edit_set(UI_TextEditState *state, String8 s);
internal void    ui_text_edit_set_u16(UI_TextEditState *state, U16 value);
internal String8 ui_text_edit_str8(UI_TextEditState *state); // view into state->buffer - copy before it outlives this frame

// Draws a single-line text field and, while `focused`, consumes this
// frame's Char/KeyDown events from `events` to edit `state` in place
// (insert at cursor, backspace/delete, left/right/home/end). `mask`
// renders every character as `*` (the password field) without changing
// what's actually stored. Focus is not managed here - the caller (the
// Connections modal) owns one `focused_field` index and passes `focused`
// in; this just reports whether the field was clicked this frame; the
// caller reacts to that by handing focus to it.
internal B32 ui_text_edit(WM_EventList *events, FP_Font *font, F32 x, F32 y, F32 w, F32 h,
                           UI_TextEditState *state, B32 focused, B32 mask);

// Hover-highlighted icon button (an atlas-packed icon, e.g. from ui_icons.h)
// at a caller-given UV rect. Returns whether it was clicked this frame.
internal B32 ui_icon_button(F32 x, F32 y, F32 size, F32 u0, F32 v0, F32 u1, F32 v1);

#endif // UI_CORE_H
