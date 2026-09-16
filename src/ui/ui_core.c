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
