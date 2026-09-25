internal U16
ui_conn_modal__parse_port(UI_TextEditState *port_state)
{
  char cstr[8] = {0};
  U64 n = Min(port_state->len, sizeof(cstr) - 1);
  MemoryCopy(cstr, port_state->buffer, n);
  int parsed = atoi(cstr);
  U16 result = (parsed > 0 && parsed <= 65535) ? (U16)parsed : 22;
  return result;
}

internal void
ui_conn_modal__load_fields(UI_ConnModal *modal, S32 index)
{
  if(index >= 0 && (U64)index < modal->profiles.count)
  {
    CONN_Profile *p = &modal->profiles.v[index];
    ui_text_edit_set(&modal->name, p->name);
    ui_text_edit_set(&modal->host, p->host);
    ui_text_edit_set_u16(&modal->port, p->port);
    ui_text_edit_set(&modal->username, p->username);
    ui_text_edit_set(&modal->password, p->password);
  }
  else
  {
    ui_text_edit_set(&modal->name, str8_lit(""));
    ui_text_edit_set(&modal->host, str8_lit(""));
    ui_text_edit_set_u16(&modal->port, 22);
    ui_text_edit_set(&modal->username, str8_lit(""));
    ui_text_edit_set(&modal->password, str8_lit(""));
  }
}

internal void
ui_conn_modal_init(UI_ConnModal *modal, Arena *persist_arena)
{
  MemoryZeroStruct(modal);
  modal->selected_index = -1;
  modal->focused_field = -1;
  modal->profiles = conn_store_load(persist_arena);
}

internal B32
ui_conn_modal_update(UI_ConnModal *modal, Arena *persist_arena, Arena *creds_arena,
                      WM_EventList *events, FP_Font *font, F32 win_w, F32 win_h,
                      String8 *out_host, U16 *out_port, String8 *out_user, String8 *out_pass)
{
  B32 connect_requested = 0;
  if(!modal->open)
  {
    return connect_requested;
  }

  // Tab cycles focus between the 5 fields, Escape closes - handled once
  // here rather than inside ui_text_edit, since these two keys are about
  // the modal's focus/lifecycle, not any one field's own editing.
  for(WM_Event *ev = events->first; ev != 0; ev = ev->next)
  {
    if(ev->kind == WM_EventKind_KeyDown)
    {
      if(ev->code == VK_TAB)
      {
        modal->focused_field = (modal->focused_field + 1) % 5;
      }
      else if(ev->code == VK_ESCAPE)
      {
        modal->open = 0;
      }
    }
  }
  if(!modal->open)
  {
    return connect_requested;
  }

  F32 panel_w = 640.0f, panel_h = 420.0f;
  F32 px0 = (win_w - panel_w) * 0.5f, py0 = (win_h - panel_h) * 0.5f;
  F32 px1 = px0 + panel_w, py1 = py0 + panel_h;

  // Dim everything behind the panel - alpha blending already works for
  // ordinary rects (glyphs already rely on it), so this is just another
  // dr_rect at a<1.
  dr_rect(0, 0, win_w, win_h, 0.0f, 0.0f, 0.0f, 0.55f);
  dr_rect(px0, py0, px1, py1, 0.13f, 0.13f, 0.15f, 1.0f);

  dr_text(font, 18.0f, px0 + 16.0f, ui_text_baseline_y(py0, 40.0f, 18.0f), 0.95f, 0.95f, 0.95f, 1.0f, str8_lit("Connections"));

  // --- saved-profile list, left column ---------------------------------------
  F32 list_x0 = px0 + 16.0f, list_x1 = px0 + 196.0f;
  F32 list_y0 = py0 + 50.0f, list_y1 = py1 - 50.0f;
  dr_rect(list_x0, list_y0, list_x1, list_y1, 0.10f, 0.10f, 0.12f, 1.0f);
  {
    F32 row_h = 24.0f;
    B32 pressed = ui__mouse_pressed_edge();
    for(U64 i = 0; i < modal->profiles.count; i += 1)
    {
      F32 ry0 = list_y0 + (F32)i * row_h, ry1 = ry0 + row_h;
      if(ry1 > list_y1)
      {
        break; // no scrolling in this pass - a handful of profiles is expected to fit
      }
      B32 hovered = ui__point_in_rect(ui_g_mouse_x, ui_g_mouse_y, list_x0, ry0, list_x1, ry1);
      B32 is_selected = ((S32)i == modal->selected_index);
      if(is_selected || hovered)
      {
        F32 bg = is_selected ? 0.24f : 0.17f;
        dr_rect(list_x0, ry0, list_x1, ry1, bg, bg, bg, 1.0f);
      }
      dr_text(font, 14.0f, list_x0 + 6.0f, ui_text_baseline_y(ry0, row_h, 14.0f), 0.85f, 0.85f, 0.85f, 1.0f,
              modal->profiles.v[i].name);
      if(hovered && pressed)
      {
        modal->selected_index = (S32)i;
        ui_conn_modal__load_fields(modal, (S32)i);
        modal->focused_field = -1;
      }
    }
  }

  // --- fields, right column ---------------------------------------------------
  F32 field_x0 = px0 + 220.0f, field_w = panel_w - 220.0f - 20.0f;
  F32 field_h = 28.0f, field_gap = 40.0f;
  String8 field_labels[5] = { str8_lit("Name"), str8_lit("Host"), str8_lit("Port"), str8_lit("Username"), str8_lit("Password") };
  UI_TextEditState *field_states[5] = { &modal->name, &modal->host, &modal->port, &modal->username, &modal->password };
  B32 field_mask[5] = { 0, 0, 0, 0, 1 };

  for(U64 i = 0; i < 5; i += 1)
  {
    F32 fy = py0 + 50.0f + (F32)i * field_gap;
    dr_text(font, 12.0f, field_x0, ui_text_baseline_y(fy, 16.0f, 12.0f), 0.55f, 0.55f, 0.55f, 1.0f, field_labels[i]);
    B32 focused = (modal->focused_field == (S32)i);
    B32 clicked = ui_text_edit(events, font, field_x0, fy + 16.0f, field_w, field_h, field_states[i], focused, field_mask[i]);
    if(clicked)
    {
      modal->focused_field = (S32)i;
    }
  }

  // --- buttons ------------------------------------------------------------------
  F32 btn_y = py1 - 40.0f, btn_h = 28.0f, btn_w = 90.0f, btn_gap = 8.0f;
  F32 btn_x = px0 + 16.0f;

  if(ui_button(font, btn_x, btn_y, btn_w, btn_h, str8_lit("New")))
  {
    modal->selected_index = -1;
    ui_conn_modal__load_fields(modal, -1);
    modal->focused_field = 0;
  }
  btn_x += btn_w + btn_gap;

  if(ui_button(font, btn_x, btn_y, btn_w, btn_h, str8_lit("Save")))
  {
    String8 name_s = str8_copy(persist_arena, ui_text_edit_str8(&modal->name));
    String8 host_s = str8_copy(persist_arena, ui_text_edit_str8(&modal->host));
    String8 user_s = str8_copy(persist_arena, ui_text_edit_str8(&modal->username));
    String8 pass_s = str8_copy(persist_arena, ui_text_edit_str8(&modal->password));
    U16 port_v = ui_conn_modal__parse_port(&modal->port);

    if(modal->selected_index >= 0 && (U64)modal->selected_index < modal->profiles.count)
    {
      CONN_Profile *p = &modal->profiles.v[modal->selected_index];
      p->name = name_s; p->host = host_s; p->port = port_v; p->username = user_s; p->password = pass_s;
    }
    else if(modal->profiles.count < CONN_MAX_PROFILES)
    {
      CONN_Profile *p = &modal->profiles.v[modal->profiles.count];
      p->name = name_s; p->host = host_s; p->port = port_v; p->username = user_s; p->password = pass_s;
      modal->selected_index = (S32)modal->profiles.count;
      modal->profiles.count += 1;
    }
    conn_store_save(&modal->profiles);
  }
  btn_x += btn_w + btn_gap;

  if(ui_button(font, btn_x, btn_y, btn_w, btn_h, str8_lit("Delete")))
  {
    if(modal->selected_index >= 0 && (U64)modal->selected_index < modal->profiles.count)
    {
      for(U64 i = (U64)modal->selected_index; i + 1 < modal->profiles.count; i += 1)
      {
        modal->profiles.v[i] = modal->profiles.v[i + 1];
      }
      modal->profiles.count -= 1;
      modal->selected_index = -1;
      ui_conn_modal__load_fields(modal, -1);
      conn_store_save(&modal->profiles);
    }
  }
  btn_x += btn_w + btn_gap;

  if(ui_button(font, btn_x, btn_y, btn_w, btn_h, str8_lit("Connect")))
  {
    *out_host = str8_copy(creds_arena, ui_text_edit_str8(&modal->host));
    *out_port = ui_conn_modal__parse_port(&modal->port);
    *out_user = str8_copy(creds_arena, ui_text_edit_str8(&modal->username));
    *out_pass = str8_copy(creds_arena, ui_text_edit_str8(&modal->password));
    connect_requested = 1;
    modal->open = 0;
  }
  btn_x += btn_w + btn_gap;

  if(ui_button(font, btn_x, btn_y, btn_w, btn_h, str8_lit("Close")))
  {
    modal->open = 0;
  }

  return connect_requested;
}
