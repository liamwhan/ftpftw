internal void
ui_settings_modal_init(UI_SettingsModal *modal)
{
  MemoryZeroStruct(modal);
}

internal void
ui_settings_modal__stepper_row(FP_Font *font, F32 x, F32 y, F32 row_w, String8 label,
                                 U32 *value, XFER_Settings *settings)
{
  dr_text(font, 14.0f, x, ui_text_baseline_y(y, 24.0f, 14.0f), 0.85f, 0.85f, 0.85f, 1.0f, label);

  F32 btn_w = 28.0f, btn_h = 24.0f, num_w = 44.0f, gap = 8.0f;
  F32 plus_x = x + row_w - btn_w;
  F32 num_x = plus_x - gap - num_w;
  F32 minus_x = num_x - gap - btn_w;

  if(ui_button(font, minus_x, y, btn_w, btn_h, str8_lit("-")))
  {
    if(*value > 1)
    {
      *value -= 1;
    }
    xfer_settings_save(settings);
  }

  char num_cstr[8];
  snprintf(num_cstr, sizeof(num_cstr), "%u", *value);
  String8 num_str = str8_cstring(num_cstr);
  F32 num_advance = 0.0f;
  FNT_Piece measure[8];
  fnt_text_pieces(font, 14.0f, num_str, 0, 0, measure, ArrayCount(measure), &num_advance);
  dr_text(font, 14.0f, num_x + (num_w - num_advance) * 0.5f, ui_text_baseline_y(y, btn_h, 14.0f),
          0.95f, 0.95f, 0.95f, 1.0f, num_str);

  if(ui_button(font, plus_x, y, btn_w, btn_h, str8_lit("+")))
  {
    if(*value < XFER_SETTINGS_HARD_CAP)
    {
      *value += 1;
    }
    xfer_settings_save(settings);
  }
}

internal void
ui_settings_modal_update(UI_SettingsModal *modal, XFER_Settings *settings, FP_Font *font, F32 win_w, F32 win_h)
{
  if(!modal->open)
  {
    return;
  }

  F32 panel_w = 380.0f, panel_h = 200.0f;
  F32 px0 = (win_w - panel_w) * 0.5f, py0 = (win_h - panel_h) * 0.5f;
  F32 px1 = px0 + panel_w, py1 = py0 + panel_h;

  dr_rect(0, 0, win_w, win_h, 0.0f, 0.0f, 0.0f, 0.55f);
  dr_rect(px0, py0, px1, py1, 0.13f, 0.13f, 0.15f, 1.0f);

  dr_text(font, 18.0f, px0 + 16.0f, ui_text_baseline_y(py0, 40.0f, 18.0f), 0.95f, 0.95f, 0.95f, 1.0f,
          str8_lit("Transfer Settings"));

  F32 row_w = panel_w - 32.0f;
  ui_settings_modal__stepper_row(font, px0 + 16.0f, py0 + 60.0f, row_w,
                                   str8_lit("Max simultaneous downloads"), &settings->max_downloads, settings);
  ui_settings_modal__stepper_row(font, px0 + 16.0f, py0 + 100.0f, row_w,
                                   str8_lit("Max simultaneous uploads"), &settings->max_uploads, settings);

  dr_text(font, 12.0f, px0 + 16.0f, ui_text_baseline_y(py0 + 138.0f, 20.0f, 12.0f), 0.5f, 0.5f, 0.5f, 1.0f,
          str8_lit("Hard cap: 10 each"));

  F32 btn_w = 90.0f, btn_h = 28.0f;
  if(ui_button(font, px1 - 16.0f - btn_w, py1 - 40.0f, btn_w, btn_h, str8_lit("Close")))
  {
    modal->open = 0;
  }
}
