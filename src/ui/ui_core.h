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
  S32 shift_anchor_index; // range-select anchor - set by plain/ctrl clicks, held fixed across shift-clicks
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
//
// `selected` is a caller-owned array of `entry_count` bools this widget
// reads AND writes (not internal state) - plain click selects only the
// clicked row *unless it's already selected*, in which case the existing
// (possibly multi-row) selection is left untouched - this is what lets a
// caller start dragging a whole multi-selection from a plain press on any
// one of its rows, rather than the press itself collapsing the selection
// down to one row before the drag begins. To shrink a multi-selection to
// one item, click a row that isn't already selected. Ctrl+click toggles
// just the clicked row regardless; Shift+click selects the contiguous
// range from `state->shift_anchor_index` to the clicked row (replacing
// any previous range; the anchor itself only moves on plain/Ctrl clicks).
// Caller should size/allocate `selected` from the same arena its
// `entries` come from, so a rebuilt listing naturally starts with a
// fresh (zeroed) selection rather than a stale one.
//
// A right-click on a row reports that row's index via *out_right_clicked
// (-1 if the right-click didn't land on a row, or none happened this
// frame) and applies the same "leave a multi-selection alone, otherwise
// collapse to just this row" rule as a plain left click, so a caller can
// open a context menu that acts on the existing multi-selection when the
// right-click landed on one of its rows. Never moves the shift-anchor
// like a left click on a fresh row does, and never touches the
// double-click timer - it's purely a selection nudge plus a "here's what
// was right-clicked" report.
internal S32 ui_row_list(Arena *frame_arena, FP_Font *font, F32 x, F32 y, F32 width, F32 height, F32 row_h,
                          FS_Entry *entries, U64 entry_count, B32 *selected,
                          UI_RowListState *state, B32 *out_double_clicked, S32 *out_right_clicked,
                          B32 input_gated);

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

// A plain hover-highlighted, centered-label button. Returns whether it was
// clicked this frame. (ui_conn_modal.c has its own near-identical private
// helper predating this - not worth churning that working code just to
// converge on this one for a single-phase addition; new modals should use
// this shared one instead of writing a third copy.)
internal B32 ui_button(FP_Font *font, F32 x, F32 y, F32 w, F32 h, String8 label);

// A right-click context menu's persistent state - caller-owned, one
// instance per "kind" of thing that can be right-clicked (e.g. one for
// transfer queue rows), same ownership model as UI_RowListState. `tag` is
// whatever the caller needs to remember *what* was right-clicked (an op
// index, ...) - this widget never reads it, just carries it across the
// open->click round trip. `screen_w`/`screen_h` should be set to the
// current window size each time the caller opens the menu, so it can keep
// itself on-screen.
typedef struct UI_ContextMenuState UI_ContextMenuState;
struct UI_ContextMenuState
{
  B32 open;
  F32 x, y; // anchor point (top-left) - the right-click location
  F32 screen_w, screen_h;
  U64 tag;
};

// Draws/hit-tests a popup menu while state->open is true (no-op otherwise).
// The caller opens it (state->open = 1, x/y/screen_w/screen_h/tag set) in
// response to a right-click; this handles everything from there on -
// hovering, closing on any click, and reporting which row (if any) was
// clicked. See UI_ContextMenuState above for the ownership model.
internal S32 ui_context_menu(FP_Font *font, UI_ContextMenuState *state, String8 *labels, U32 label_count);

// A simple Yes/Cancel confirmation panel - same dimmed-backdrop-plus-panel
// chrome as the Connections/Settings modals, but small and single-purpose
// (currently: "are you sure you want to delete this?"). Caller-owned state,
// same ownership model as UI_ContextMenuState above. The message is copied
// into a small fixed buffer (see ui_confirm_open) rather than referenced by
// pointer, so it stays valid across however many frames the dialog is open
// without needing its own persistent arena.
typedef struct UI_ConfirmState UI_ConfirmState;
struct UI_ConfirmState
{
  B32 open;
  // 512, not the original 192 - big enough to hold main.c's host-key
  // confirmation message (a host name, port, and a 95-character
  // colon-grouped SHA256 fingerprint the user actually needs to be able
  // to read in full, not have silently truncated) as well as the
  // original short delete-confirmation messages.
  U8  message[512];
  U64 message_size;
};

// Opens the dialog with `message` (truncated to fit the fixed buffer above).
internal void ui_confirm_open(UI_ConfirmState *state, String8 message);

// Draws/hit-tests the panel while state->open is true (no-op otherwise).
// Returns 1 if `confirm_label` was clicked this frame; Cancel also closes
// it (state->open = 0) but returns 0. Unlike ui_context_menu, a click
// outside the panel does NOT dismiss it - only the two buttons do - since
// this is typically opened from a click that itself lands outside the
// panel (a context-menu choice), and checking for that on the same frame
// would dismiss it before it's ever seen.
internal B32 ui_confirm_dialog(FP_Font *font, UI_ConfirmState *state, F32 win_w, F32 win_h, String8 confirm_label);

#endif // UI_CORE_H
