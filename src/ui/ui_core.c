#define UI_DOUBLE_CLICK_US 400000 // 400ms

global F32 ui_g_mouse_x = 0.0f;
global F32 ui_g_mouse_y = 0.0f;
global B32 ui_g_mouse_left_down = 0;
global B32 ui_g_mouse_left_was_down = 0; // previous frame's value, for edge detection
global B32 ui_g_mouse_right_down = 0;
global B32 ui_g_mouse_right_was_down = 0;
global B32 ui_g_ctrl_down = 0;
global B32 ui_g_shift_down = 0;

internal void
ui_begin_frame(WM_Window window)
{
  wm_mouse_state(window, &ui_g_mouse_x, &ui_g_mouse_y, &ui_g_mouse_left_down);
  ui_g_mouse_right_down = wm_mouse_right_down();
  B32 alt_unused;
  wm_key_modifiers(&ui_g_ctrl_down, &ui_g_shift_down, &alt_unused);
}

internal void
ui_end_frame(void)
{
  ui_g_mouse_left_was_down = ui_g_mouse_left_down;
  ui_g_mouse_right_was_down = ui_g_mouse_right_down;
}

internal F32
ui_text_baseline_y(F32 box_top, F32 box_h, F32 size)
{
  return box_top + box_h * 0.5f + size * 0.35f;
}

internal B32
ui__mouse_pressed_edge(void)
{
  return ui_g_mouse_left_down && !ui_g_mouse_left_was_down;
}

internal B32
ui__mouse_right_pressed_edge(void)
{
  return ui_g_mouse_right_down && !ui_g_mouse_right_was_down;
}

internal B32
ui__point_in_rect(F32 px, F32 py, F32 x0, F32 y0, F32 x1, F32 y1)
{
  return px >= x0 && px < x1 && py >= y0 && py < y1;
}

// Shared skeuomorphic-lite button chrome: a flat base fill plus a thin
// lighter top edge / darker bottom edge to read as subtly raised, swapped
// (dark top / light bottom) when pressed to read as pushed in. Modeled on
// how raddebugger fakes its own button bevel - layered flat-alpha strips,
// not an actual gradient (their renderer supports real per-corner
// gradients; ours doesn't need to, since this is what RAD's own button
// bevel actually is under the hood). Every button-like widget in this file
// funnels through here so the look stays consistent app-wide.
internal void
ui_draw_button_bg(F32 x0, F32 y0, F32 x1, F32 y1, F32 base_r, F32 base_g, F32 base_b, B32 hovered, B32 pressed)
{
  F32 bump = hovered ? 0.05f : 0.0f;
  F32 r = Clamp(0.0f, base_r + bump, 1.0f);
  F32 g = Clamp(0.0f, base_g + bump, 1.0f);
  F32 b = Clamp(0.0f, base_b + bump, 1.0f);
  dr_rect(x0, y0, x1, y1, r, g, b, 1.0f);

  F32 edge = Min(2.0f, (y1 - y0) * 0.18f); // thin - proportional to height, capped at 2px
  F32 light_a = 0.18f, dark_a = 0.22f;
  if(!pressed)
  {
    dr_rect(x0, y0, x1, y0 + edge, 1.0f, 1.0f, 1.0f, light_a); // top highlight
    dr_rect(x0, y1 - edge, x1, y1, 0.0f, 0.0f, 0.0f, dark_a);  // bottom shadow
  }
  else
  {
    dr_rect(x0, y0, x1, y0 + edge, 0.0f, 0.0f, 0.0f, dark_a);  // top shadow (pushed in)
    dr_rect(x0, y1 - edge, x1, y1, 1.0f, 1.0f, 1.0f, light_a); // bottom highlight
  }

  // subtle side borders for definition - top/bottom already framed by the
  // bevel strips above, so only the sides need it
  dr_rect(x0, y0, x0 + 1.0f, y1, 0.0f, 0.0f, 0.0f, 0.30f);
  dr_rect(x1 - 1.0f, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.30f);
}

internal S32
ui_tab_strip(FP_Font *font, F32 x, F32 y, F32 tab_w, F32 tab_h,
              String8 *labels, U32 label_count, S32 selected)
{
  S32 clicked = -1;
  B32 pressed = ui__mouse_pressed_edge();

  for(U32 i = 0; i < label_count; i += 1)
  {
    F32 tx0 = x + (F32)i * tab_w;
    F32 tx1 = tx0 + tab_w;
    F32 ty0 = y;
    F32 ty1 = y + tab_h;
    B32 hovered = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, tx0, ty0, tx1, ty1);
    B32 is_selected = ((S32)i == selected);
    B32 pressed_visual = hovered && ui_g_mouse_left_down;

    F32 bg = is_selected ? 0.22f : 0.13f;
    ui_draw_button_bg(tx0, ty0, tx1, ty1, bg, bg, bg, hovered && !is_selected, pressed_visual);

    F32 tc = is_selected ? 1.0f : 0.7f;
    dr_text(font, 15.0f, tx0 + 12.0f, ui_text_baseline_y(ty0, tab_h, 15.0f), tc, tc, tc, 1.0f, labels[i]);

    if(hovered && pressed)
    {
      clicked = (S32)i;
    }
  }

  return clicked;
}

internal S32
ui_row_list(Arena *frame_arena, FP_Font *font, F32 x, F32 y, F32 width, F32 height, F32 row_h,
             FS_Entry *entries, U64 entry_count, B32 *selected,
             UI_RowListState *state, B32 *out_double_clicked, S32 *out_right_clicked,
             B32 input_gated)
{
  S32 clicked = -1;
  *out_double_clicked = 0;
  *out_right_clicked = -1;
  // Gated (a modal - e.g. a delete confirm - is open): a click meant for the
  // modal must never fall through and re-select whatever pane row happens
  // to sit under the mouse, or a confirm click would silently delete
  // whatever's under the cursor instead of the originally-selected file(s).
  B32 pressed = !input_gated && ui__mouse_pressed_edge();
  B32 right_pressed = !input_gated && ui__mouse_right_pressed_edge();
  B32 mouse_in_pane = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, x, y, x + width, y + height);

  F32 content_h = (F32)entry_count * row_h;
  F32 max_scroll = Max(0.0f, content_h - height);
  if(mouse_in_pane)
  {
    F32 wheel = wm_mouse_wheel_delta();
    // wheel forward/up -> content moves toward the top (standard convention)
    state->scroll_y -= wheel * row_h * 3.0f;
  }
  state->scroll_y = Clamp(0.0f, state->scroll_y, max_scroll);

  dr_set_clip(x, y, x + width, y + height);

  F32 icon_dim = Min(row_h - 6.0f, 16.0f);

  for(U64 i = 0; i < entry_count; i += 1)
  {
    F32 ry0 = y + (F32)i * row_h - state->scroll_y;
    F32 ry1 = ry0 + row_h;
    if(ry1 < y || ry0 > y + height)
    {
      continue; // fully outside the visible pane - clip would hide it anyway, this just saves instances
    }
    B32 hovered = mouse_in_pane && ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, x, ry0, x + width, ry1);
    B32 is_selected = selected[i];

    if(is_selected)
    {
      dr_rect(x, ry0, x + width, ry1, 0.20f, 0.32f, 0.45f, 1.0f);
    }
    else if(hovered)
    {
      dr_rect(x, ry0, x + width, ry1, 0.18f, 0.18f, 0.20f, 1.0f);
    }

    FS_Entry *e = &entries[i];
    F32 icon_y = ry0 + (row_h - icon_dim) * 0.5f;
    F32 u0, v0, u1, v1;
    ui_icon_uv(e->is_dir, &u0, &v0, &u1, &v1);
    F32 tint_r = e->is_dir ? 0.95f : 0.75f;
    F32 tint_g = e->is_dir ? 0.78f : 0.75f;
    F32 tint_b = e->is_dir ? 0.30f : 0.80f;
    dr_image(x + 6.0f, icon_y, x + 6.0f + icon_dim, icon_y + icon_dim, u0, v0, u1, v1, tint_r, tint_g, tint_b, 1.0f);

    F32 text_x = x + 6.0f + icon_dim + 8.0f;
    F32 text_y = ui_text_baseline_y(ry0, row_h, 15.0f);
    dr_text(font, 15.0f, text_x, text_y, 0.85f, 0.85f, 0.85f, 1.0f, e->name);

    if(!e->is_dir)
    {
      String8 size_str = str8f(frame_arena, "%llu", e->size);
      dr_text(font, 13.0f, x + width - 90.0f, ui_text_baseline_y(ry0, row_h, 13.0f), 0.55f, 0.55f, 0.55f, 1.0f, size_str);
    }

    if(hovered && pressed)
    {
      clicked = (S32)i;
      U64 now = now_time_us();
      if(state->last_click_index == (S32)i && (now - state->last_click_time_us) < UI_DOUBLE_CLICK_US)
      {
        *out_double_clicked = 1;
      }
      state->last_click_index = (S32)i;
      state->last_click_time_us = now;

      if(ui_g_ctrl_down)
      {
        selected[i] = !selected[i];
        state->shift_anchor_index = (S32)i;
      }
      else if(ui_g_shift_down)
      {
        S32 anchor = (state->shift_anchor_index >= 0) ? state->shift_anchor_index : (S32)i;
        S32 lo = Min(anchor, (S32)i);
        S32 hi = Max(anchor, (S32)i);
        for(U64 j = 0; j < entry_count; j += 1)
        {
          selected[j] = ((S32)j >= lo && (S32)j <= hi);
        }
        // anchor deliberately not moved - repeated shift-clicks extend/shrink from the same start
      }
      else
      {
        // Plain click selects only this row - UNLESS it's already part of
        // a multi-selection, in which case leave the selection untouched.
        // Without this, pressing down on one of several already-selected
        // rows to start dragging the whole group would collapse the
        // selection to just that one row before the drag even begins
        // (this click-handling runs before the caller's own drag-arming
        // logic sees the selection). To shrink a multi-selection down to
        // one item, click a row that isn't already selected.
        if(!selected[i])
        {
          for(U64 j = 0; j < entry_count; j += 1)
          {
            selected[j] = 0;
          }
          selected[i] = 1;
        }
        state->shift_anchor_index = (S32)i;
      }
    }

    if(hovered && right_pressed)
    {
      *out_right_clicked = (S32)i;
      // Same collapse-unless-already-selected rule as the plain left click
      // above, so right-clicking one row of an existing multi-selection
      // opens a context menu that acts on the whole selection, while
      // right-clicking outside it acts on just that one row.
      if(!selected[i])
      {
        for(U64 j = 0; j < entry_count; j += 1)
        {
          selected[j] = 0;
        }
        selected[i] = 1;
        state->shift_anchor_index = (S32)i;
      }
    }
  }

  dr_clear_clip();

  return clicked;
}

internal void
ui_text_edit_set(UI_TextEditState *state, String8 s)
{
  U64 n = Min(s.size, sizeof(state->buffer) - 1);
  MemoryCopy(state->buffer, s.str, n);
  state->len = n;
  state->cursor = n;
}

internal void
ui_text_edit_set_u16(UI_TextEditState *state, U16 value)
{
  char buf[8];
  int n = snprintf(buf, sizeof(buf), "%u", (unsigned)value);
  U64 un = (U64)ClampBot(0, n);
  un = Min(un, sizeof(state->buffer) - 1);
  MemoryCopy(state->buffer, buf, un);
  state->len = un;
  state->cursor = un;
}

internal String8
ui_text_edit_str8(UI_TextEditState *state)
{
  return str8(state->buffer, state->len);
}

internal B32
ui_text_edit(WM_EventList *events, FP_Font *font, F32 x, F32 y, F32 w, F32 h,
              UI_TextEditState *state, B32 focused, B32 mask)
{
  B32 pressed = ui__mouse_pressed_edge();
  B32 hovered = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, x, y, x + w, y + h);
  B32 clicked = hovered && pressed;

  if(focused)
  {
    for(WM_Event *ev = events->first; ev != 0; ev = ev->next)
    {
      if(ev->kind == WM_EventKind_Char)
      {
        if(state->len < sizeof(state->buffer) - 1)
        {
          MemoryCopy(state->buffer + state->cursor + 1, state->buffer + state->cursor, state->len - state->cursor);
          state->buffer[state->cursor] = (U8)ev->code;
          state->len += 1;
          state->cursor += 1;
        }
      }
      else if(ev->kind == WM_EventKind_KeyDown)
      {
        switch(ev->code)
        {
          case VK_BACK:
          {
            if(state->cursor > 0)
            {
              MemoryCopy(state->buffer + state->cursor - 1, state->buffer + state->cursor, state->len - state->cursor);
              state->cursor -= 1;
              state->len -= 1;
            }
          }break;
          case VK_DELETE:
          {
            if(state->cursor < state->len)
            {
              MemoryCopy(state->buffer + state->cursor, state->buffer + state->cursor + 1, state->len - state->cursor - 1);
              state->len -= 1;
            }
          }break;
          case VK_LEFT:  { if(state->cursor > 0) { state->cursor -= 1; } }break;
          case VK_RIGHT: { if(state->cursor < state->len) { state->cursor += 1; } }break;
          case VK_HOME:  { state->cursor = 0; }break;
          case VK_END:   { state->cursor = state->len; }break;
          default: break;
        }
      }
      else if(ev->kind == WM_EventKind_Paste)
      {
        U8 paste_buf[256];
        U64 paste_len = wm_clipboard_get_text(paste_buf, sizeof(paste_buf));
        U64 space = (sizeof(state->buffer) - 1) - state->len;
        U64 insert_len = Min(paste_len, space);
        if(insert_len > 0)
        {
          MemoryCopy(state->buffer + state->cursor + insert_len, state->buffer + state->cursor, state->len - state->cursor);
          MemoryCopy(state->buffer + state->cursor, paste_buf, insert_len);
          state->len += insert_len;
          state->cursor += insert_len;
        }
      }
    }
  }

  F32 bg = focused ? 0.20f : (hovered ? 0.16f : 0.13f);
  dr_rect(x, y, x + w, y + h, bg, bg, bg, 1.0f);

  U8 mask_buf[256];
  String8 display;
  if(mask)
  {
    MemorySet(mask_buf, '*', state->len);
    display = str8(mask_buf, state->len);
  }
  else
  {
    display = str8(state->buffer, state->len);
  }
  dr_text(font, 14.0f, x + 6.0f, ui_text_baseline_y(y, h, 14.0f), 0.9f, 0.9f, 0.9f, 1.0f, display);

  if(focused)
  {
    F32 cursor_advance = 0.0f;
    if(state->cursor > 0)
    {
      FNT_Piece measure_pieces[300];
      String8 pre = mask ? str8(mask_buf, state->cursor) : str8(state->buffer, state->cursor);
      fnt_text_pieces(font, 14.0f, pre, 0, 0, measure_pieces, ArrayCount(measure_pieces), &cursor_advance);
    }
    F32 cursor_x = x + 6.0f + cursor_advance;
    dr_rect(cursor_x, y + 4.0f, cursor_x + 1.5f, y + h - 4.0f, 0.9f, 0.9f, 0.9f, 1.0f);
  }

  return clicked;
}

internal B32
ui_icon_button(F32 x, F32 y, F32 size, F32 u0, F32 v0, F32 u1, F32 v1)
{
  B32 hovered = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, x, y, x + size, y + size);
  B32 pressed_visual = hovered && ui_g_mouse_left_down;
  B32 clicked_edge = ui__mouse_pressed_edge();

  // Still invisible at rest (no chip floating in the toolbar until you
  // interact with it) - the bevel chrome only appears once hovered.
  if(hovered)
  {
    ui_draw_button_bg(x, y, x + size, y + size, 0.20f, 0.20f, 0.20f, 1, pressed_visual);
  }

  F32 pad = size * 0.22f;
  F32 tint = hovered ? 1.0f : 0.75f;
  dr_image(x + pad, y + pad, x + size - pad, y + size - pad, u0, v0, u1, v1, tint, tint, tint, 1.0f);

  return hovered && clicked_edge;
}

internal B32
ui_button(FP_Font *font, F32 x, F32 y, F32 w, F32 h, String8 label)
{
  B32 hovered = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, x, y, x + w, y + h);
  B32 pressed_visual = hovered && ui_g_mouse_left_down;
  B32 clicked_edge = ui__mouse_pressed_edge();
  ui_draw_button_bg(x, y, x + w, y + h, 0.20f, 0.20f, 0.20f, hovered, pressed_visual);

  F32 advance = 0.0f;
  FNT_Piece measure[64];
  fnt_text_pieces(font, 14.0f, label, 0, 0, measure, ArrayCount(measure), &advance);
  F32 text_x = x + (w - advance) * 0.5f;
  // nudge the label down 1px while pressed - reinforces the pushed-in look
  F32 text_y = ui_text_baseline_y(y, h, 14.0f) + (pressed_visual ? 1.0f : 0.0f);
  dr_text(font, 14.0f, text_x, text_y, 0.9f, 0.9f, 0.9f, 1.0f, label);

  return hovered && clicked_edge;
}

// A small popup menu, one row per label, anchored at state->x/y (top-left -
// the caller sets this to the right-click point). Only draws/hit-tests
// while state->open is true; the caller is responsible for opening it (in
// response to a right-click on whatever it's a context menu for - a row,
// an icon, etc) and for stashing whatever identifies *what* it's a menu
// for in state->tag (an op index, a row index, ...), since this widget has
// no idea what the labels mean. Any left-click this frame closes the menu -
// on one of its own rows (returns that row's index, having also been the
// thing that closed it) or anywhere else (dismiss, returns -1) - standard
// context-menu behavior. Opening is driven by a *right*-click edge
// elsewhere, so there's no same-frame conflict between "the click that
// opened it" and "the click that closes it."
internal S32
ui_context_menu(FP_Font *font, UI_ContextMenuState *state, String8 *labels, U32 label_count)
{
  S32 clicked = -1;
  if(!state->open)
  {
    return clicked;
  }

  F32 row_h = 24.0f, pad_x = 10.0f;
  F32 menu_w = 120.0f;
  for(U32 i = 0; i < label_count; i += 1)
  {
    F32 advance = 0.0f;
    FNT_Piece measure[64];
    fnt_text_pieces(font, 13.0f, labels[i], 0, 0, measure, ArrayCount(measure), &advance);
    menu_w = Max(menu_w, advance + pad_x * 2.0f);
  }
  F32 menu_h = (F32)label_count * row_h;

  // Keep the whole menu on-screen even when the right-click landed near an
  // edge - flip to open leftward/upward from the click point instead of
  // letting it run off (dr_rect has no clamping of its own).
  F32 x0 = state->x, y0 = state->y;
  if(x0 + menu_w > state->screen_w) { x0 = state->screen_w - menu_w; }
  if(y0 + menu_h > state->screen_h) { y0 = state->screen_h - menu_h; }
  x0 = Max(0.0f, x0);
  y0 = Max(0.0f, y0);
  F32 x1 = x0 + menu_w, y1 = y0 + menu_h;

  B32 pressed = ui__mouse_pressed_edge();

  dr_rect(x0, y0, x1, y1, 0.15f, 0.15f, 0.16f, 1.0f);
  dr_rect(x0, y0, x1, y0 + 1.0f, 0.35f, 0.35f, 0.38f, 1.0f); // top edge
  dr_rect(x0, y1 - 1.0f, x1, y1, 0.0f, 0.0f, 0.0f, 0.45f);   // bottom edge
  dr_rect(x0, y0, x0 + 1.0f, y1, 0.35f, 0.35f, 0.38f, 1.0f); // left edge
  dr_rect(x1 - 1.0f, y0, x1, y1, 0.0f, 0.0f, 0.0f, 0.45f);   // right edge

  for(U32 i = 0; i < label_count; i += 1)
  {
    F32 ry0 = y0 + (F32)i * row_h, ry1 = ry0 + row_h;
    B32 hovered = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, x0, ry0, x1, ry1);
    if(hovered)
    {
      dr_rect(x0, ry0, x1, ry1, 0.24f, 0.32f, 0.42f, 1.0f);
    }
    dr_text(font, 13.0f, x0 + pad_x, ui_text_baseline_y(ry0, row_h, 13.0f), 0.9f, 0.9f, 0.9f, 1.0f, labels[i]);
    if(hovered && pressed)
    {
      clicked = (S32)i;
    }
  }

  if(pressed)
  {
    state->open = 0; // any click this frame dismisses it - item chosen or not
  }

  return clicked;
}

internal void
ui_confirm_open(UI_ConfirmState *state, String8 message)
{
  state->open = 1;
  state->message_size = Min(message.size, sizeof(state->message));
  MemoryCopy(state->message, message.str, state->message_size);
}

internal B32
ui_confirm_dialog(FP_Font *font, UI_ConfirmState *state, F32 win_w, F32 win_h, String8 confirm_label)
{
  B32 confirmed = 0;
  if(!state->open)
  {
    return confirmed;
  }

  // The message can carry embedded '\n's (see main.c's host-key
  // confirmation, which needs the server/port on one line and a
  // 95-character fingerprint the user actually has to be able to read
  // in full on another) - dr_text has no line-wrapping or multi-line
  // layout of its own (single fnt_text_pieces run along one baseline),
  // so this widget splits on '\n' itself and draws each segment on its
  // own line. A plain single-line message (every delete-confirmation
  // caller today) is just the degenerate one-line case.
  String8 message = str8(state->message, state->message_size);
  F32 msg_font_size = 13.0f, msg_line_h = 18.0f;
  String8 msg_lines[16];
  U32 msg_line_count = 0;
  {
    U64 start = 0;
    for(U64 i = 0; i <= message.size && msg_line_count < ArrayCount(msg_lines); i += 1)
    {
      if(i == message.size || message.str[i] == '\n')
      {
        msg_lines[msg_line_count] = str8(message.str + start, i - start);
        msg_line_count += 1;
        start = i + 1;
      }
    }
  }

  // Measure the actual widest line (rather than guessing an average
  // char width) so a long one - the 95-character colon-grouped SHA256
  // fingerprint line, in particular - is never silently clipped by
  // dr_set_clip below; this widget has no line-wrapping of its own. A
  // plain one-line delete-confirmation message just measures short and
  // gets the 360 floor.
  F32 widest_line = 0.0f;
  for(U32 i = 0; i < msg_line_count; i += 1)
  {
    F32 line_advance = 0.0f;
    FNT_Piece measure_pieces[300];
    fnt_text_pieces(font, msg_font_size, msg_lines[i], 0, 0, measure_pieces, ArrayCount(measure_pieces), &line_advance);
    widest_line = Max(widest_line, line_advance);
  }
  F32 panel_w = Clamp(360.0f, widest_line + 32.0f, win_w - 40.0f);
  F32 msg_block_h = (F32)msg_line_count * msg_line_h;
  F32 panel_h = Clamp(120.0f, 76.0f + msg_block_h, win_h - 40.0f);
  F32 px0 = (win_w - panel_w) * 0.5f, py0 = (win_h - panel_h) * 0.5f;
  F32 px1 = px0 + panel_w, py1 = py0 + panel_h;

  // Same dim-backdrop-plus-panel chrome as the Connections/Settings modals.
  dr_rect(0, 0, win_w, win_h, 0.0f, 0.0f, 0.0f, 0.55f);
  dr_rect(px0, py0, px1, py1, 0.13f, 0.13f, 0.15f, 1.0f);

  dr_set_clip(px0 + 8.0f, py0 + 8.0f, px1 - 8.0f, py1 - 56.0f);
  for(U32 i = 0; i < msg_line_count; i += 1)
  {
    F32 line_y = py0 + 20.0f + (F32)i * msg_line_h;
    dr_text(font, msg_font_size, px0 + 16.0f, ui_text_baseline_y(line_y, msg_line_h, msg_font_size),
             0.9f, 0.9f, 0.9f, 1.0f, msg_lines[i]);
  }
  dr_clear_clip();

  F32 btn_y = py1 - 44.0f, btn_h = 28.0f, btn_w = 90.0f, btn_gap = 8.0f;
  F32 btn_x = px1 - 16.0f - btn_w * 2.0f - btn_gap;

  if(ui_button(font, btn_x, btn_y, btn_w, btn_h, str8_lit("Cancel")))
  {
    state->open = 0;
  }
  btn_x += btn_w + btn_gap;

  if(ui_button(font, btn_x, btn_y, btn_w, btn_h, confirm_label))
  {
    confirmed = 1;
    state->open = 0;
  }

  // Deliberately no "click outside the panel dismisses" behavior (unlike
  // ui_context_menu's "any click closes it") - this dialog is typically
  // opened from the same click that closes a context menu (see main.c's
  // "Delete" handler), and that originating click almost always lands
  // outside this panel's centered rect. Checking for an outside click on
  // the same frame the dialog opens would dismiss it before it's ever
  // seen, so - same as ui_conn_modal/ui_settings_modal - only the two
  // explicit buttons above close it.

  return confirmed;
}
