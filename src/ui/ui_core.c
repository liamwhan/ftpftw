#define UI_DOUBLE_CLICK_US 400000 // 400ms

global F32 ui_g_mouse_x = 0.0f;
global F32 ui_g_mouse_y = 0.0f;
global B32 ui_g_mouse_left_down = 0;
global B32 ui_g_mouse_left_was_down = 0; // previous frame's value, for edge detection

internal void
ui_begin_frame(WM_Window window)
{
  wm_mouse_state(window, &ui_g_mouse_x, &ui_g_mouse_y, &ui_g_mouse_left_down);
}

internal void
ui_end_frame(void)
{
  ui_g_mouse_left_was_down = ui_g_mouse_left_down;
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
ui__point_in_rect(F32 px, F32 py, F32 x0, F32 y0, F32 x1, F32 y1)
{
  return px >= x0 && px < x1 && py >= y0 && py < y1;
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

    F32 bg = is_selected ? 0.22f : (hovered ? 0.17f : 0.13f);
    dr_rect(tx0, ty0, tx1, ty1, bg, bg, bg, 1.0f);

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
             FS_Entry *entries, U64 entry_count,
             UI_RowListState *state, B32 *out_double_clicked)
{
  S32 clicked = -1;
  *out_double_clicked = 0;
  B32 pressed = ui__mouse_pressed_edge();
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

    if(hovered)
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
  B32 pressed = ui__mouse_pressed_edge();

  if(hovered)
  {
    F32 bg = 0.22f;
    dr_rect(x, y, x + size, y + size, bg, bg, bg, 1.0f);
  }

  F32 pad = size * 0.22f;
  F32 tint = hovered ? 1.0f : 0.75f;
  dr_image(x + pad, y + pad, x + size - pad, y + size - pad, u0, v0, u1, v1, tint, tint, tint, 1.0f);

  return hovered && pressed;
}
