#define UI_ICON_DIM 16

global struct
{
  F32 folder_u0, folder_v0, folder_u1, folder_v1;
  F32 file_u0, file_v0, file_u1, file_v1;
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
