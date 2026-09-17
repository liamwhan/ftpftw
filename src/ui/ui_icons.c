#define UI_ICON_DIM 16

global struct
{
  F32 folder_u0, folder_v0, folder_u1, folder_v1;
  F32 file_u0, file_v0, file_u1, file_v1;
  F32 conn_u0, conn_v0, conn_u1, conn_v1;
  F32 settings_u0, settings_v0, settings_u1, settings_v1;
}
ui_icons_g;

internal void
ui_icons__make_folder_bitmap(U8 *pixels, U32 dim)
{
  MemoryZero(pixels, dim * dim);
  // tab, top-left
  for(U32 y = dim / 6; y < dim / 3; y += 1)
  {
    for(U32 x = 1; x < dim / 2; x += 1)
    {
      pixels[y * dim + x] = 255;
    }
  }
  // body
  for(U32 y = dim / 3; y < dim - 2; y += 1)
  {
    for(U32 x = 1; x < dim - 1; x += 1)
    {
      pixels[y * dim + x] = 255;
    }
  }
}

internal void
ui_icons__make_file_bitmap(U8 *pixels, U32 dim)
{
  MemoryZero(pixels, dim * dim);
  U32 x0 = 2, x1 = dim - 2, y0 = 1, y1 = dim - 1;
  for(U32 y = y0; y < y1; y += 1)
  {
    for(U32 x = x0; x < x1; x += 1)
    {
      pixels[y * dim + x] = 255;
    }
  }
  // folded-corner look: cut a triangular notch out of the top-right corner
  U32 notch = 4;
  for(U32 i = 0; i < notch; i += 1)
  {
    for(U32 j = 0; j < notch - i; j += 1)
    {
      U32 x = x1 - 1 - j;
      U32 y = y0 + i;
      pixels[y * dim + x] = 0;
    }
  }
}

internal void
ui_icons__make_conn_bitmap(U8 *pixels, U32 dim)
{
  // Two overlapping filled squares - a plain "things joined together" glyph
  // for the Connections toolbar button. Same flat-fill style as the
  // folder/file icons above - a placeholder look by design, easy to swap
  // for something nicer later (see CLAUDE.md on hand-drawn icons).
  MemoryZero(pixels, dim * dim);
  U32 sq = dim * 5 / 8;
  U32 off = dim / 6;
  for(U32 y = off; y < dim && y < off + sq; y += 1)
  {
    for(U32 x = 0; x < sq && x < dim; x += 1)
    {
      pixels[y * dim + x] = 255;
    }
  }
  U32 bx0 = dim - sq;
  for(U32 y = 0; y < sq && y < dim; y += 1)
  {
    for(U32 x = bx0; x < dim; x += 1)
    {
      pixels[y * dim + x] = 255;
    }
  }
}

internal void
ui_icons__make_settings_bitmap(U8 *pixels, U32 dim)
{
  // A ring (filled circle minus a smaller filled circle, via squared-
  // distance comparison - no sqrt/math.h needed) plus 4 small "teeth"
  // poking out top/bottom/left/right. Reads as a gear at icon size
  // without needing real tooth-polygon geometry.
  MemoryZero(pixels, dim * dim);
  F32 cx = (F32)dim * 0.5f, cy = (F32)dim * 0.5f;
  F32 outer_r = (F32)dim * 0.34f, inner_r = (F32)dim * 0.16f;
  F32 outer_r2 = outer_r * outer_r, inner_r2 = inner_r * inner_r;
  for(U32 y = 0; y < dim; y += 1)
  {
    for(U32 x = 0; x < dim; x += 1)
    {
      F32 dx = (F32)x + 0.5f - cx;
      F32 dy = (F32)y + 0.5f - cy;
      F32 d2 = dx * dx + dy * dy;
      if(d2 <= outer_r2 && d2 >= inner_r2)
      {
        pixels[y * dim + x] = 255;
      }
    }
  }

  U32 tooth = Max(1, (S32)(dim / 8));
  U32 mid = dim / 2, half_tooth = tooth / 2;
  for(U32 y = 0; y < tooth; y += 1) // top
  {
    for(U32 x = mid - half_tooth; x < mid + half_tooth && x < dim; x += 1) { pixels[y * dim + x] = 255; }
  }
  for(U32 y = dim - tooth; y < dim; y += 1) // bottom
  {
    for(U32 x = mid - half_tooth; x < mid + half_tooth && x < dim; x += 1) { pixels[y * dim + x] = 255; }
  }
  for(U32 x = 0; x < tooth; x += 1) // left
  {
    for(U32 y = mid - half_tooth; y < mid + half_tooth && y < dim; y += 1) { pixels[y * dim + x] = 255; }
  }
  for(U32 x = dim - tooth; x < dim; x += 1) // right
  {
    for(U32 y = mid - half_tooth; y < mid + half_tooth && y < dim; y += 1) { pixels[y * dim + x] = 255; }
  }
}

internal void
ui_icons_init(void)
{
  U8 bitmap[UI_ICON_DIM * UI_ICON_DIM];

  ui_icons__make_folder_bitmap(bitmap, UI_ICON_DIM);
  fnt_atlas_pack_bitmap(bitmap, UI_ICON_DIM, UI_ICON_DIM,
                         &ui_icons_g.folder_u0, &ui_icons_g.folder_v0,
                         &ui_icons_g.folder_u1, &ui_icons_g.folder_v1);

  ui_icons__make_file_bitmap(bitmap, UI_ICON_DIM);
  fnt_atlas_pack_bitmap(bitmap, UI_ICON_DIM, UI_ICON_DIM,
                         &ui_icons_g.file_u0, &ui_icons_g.file_v0,
                         &ui_icons_g.file_u1, &ui_icons_g.file_v1);

  ui_icons__make_conn_bitmap(bitmap, UI_ICON_DIM);
  fnt_atlas_pack_bitmap(bitmap, UI_ICON_DIM, UI_ICON_DIM,
                         &ui_icons_g.conn_u0, &ui_icons_g.conn_v0,
                         &ui_icons_g.conn_u1, &ui_icons_g.conn_v1);

  ui_icons__make_settings_bitmap(bitmap, UI_ICON_DIM);
  fnt_atlas_pack_bitmap(bitmap, UI_ICON_DIM, UI_ICON_DIM,
                         &ui_icons_g.settings_u0, &ui_icons_g.settings_v0,
                         &ui_icons_g.settings_u1, &ui_icons_g.settings_v1);
}

internal void
ui_icon_uv(B32 is_dir, F32 *out_u0, F32 *out_v0, F32 *out_u1, F32 *out_v1)
{
  if(is_dir)
  {
    *out_u0 = ui_icons_g.folder_u0; *out_v0 = ui_icons_g.folder_v0;
    *out_u1 = ui_icons_g.folder_u1; *out_v1 = ui_icons_g.folder_v1;
  }
  else
  {
    *out_u0 = ui_icons_g.file_u0; *out_v0 = ui_icons_g.file_v0;
    *out_u1 = ui_icons_g.file_u1; *out_v1 = ui_icons_g.file_v1;
  }
}

internal void
ui_icon_conn_uv(F32 *out_u0, F32 *out_v0, F32 *out_u1, F32 *out_v1)
{
  *out_u0 = ui_icons_g.conn_u0; *out_v0 = ui_icons_g.conn_v0;
  *out_u1 = ui_icons_g.conn_u1; *out_v1 = ui_icons_g.conn_v1;
}

internal void
ui_icon_settings_uv(F32 *out_u0, F32 *out_v0, F32 *out_u1, F32 *out_v1)
{
  *out_u0 = ui_icons_g.settings_u0; *out_v0 = ui_icons_g.settings_v0;
  *out_u1 = ui_icons_g.settings_u1; *out_v1 = ui_icons_g.settings_v1;
}
