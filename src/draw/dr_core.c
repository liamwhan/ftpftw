#define DR_MAX_PIECES_PER_STRING 1024

// "No clipping" - a rect large enough that no real screen coordinate ever
// falls outside it, rather than special-casing an unclipped state in the
// shader.
global F32 dr_g_clip_x0 = -1e6f, dr_g_clip_y0 = -1e6f, dr_g_clip_x1 = 1e6f, dr_g_clip_y1 = 1e6f;

internal void
dr_begin_frame(F32 clear_r, F32 clear_g, F32 clear_b, F32 clear_a)
{
  dr_clear_clip();
  r_begin_frame(clear_r, clear_g, clear_b, clear_a);
}

internal void
dr_end_frame(void)
{
  r_end_frame(fnt_atlas());
}

internal void
dr_set_clip(F32 x0, F32 y0, F32 x1, F32 y1)
{
  dr_g_clip_x0 = x0; dr_g_clip_y0 = y0; dr_g_clip_x1 = x1; dr_g_clip_y1 = y1;
}

internal void
dr_clear_clip(void)
{
  dr_g_clip_x0 = -1e6f; dr_g_clip_y0 = -1e6f; dr_g_clip_x1 = 1e6f; dr_g_clip_y1 = 1e6f;
}

internal void
dr_rect(F32 x0, F32 y0, F32 x1, F32 y1, F32 r, F32 g, F32 b, F32 a)
{
  R_RectInst inst = {0};
  inst.dst_x0 = x0; inst.dst_y0 = y0; inst.dst_x1 = x1; inst.dst_y1 = y1;
  inst.color_r = r; inst.color_g = g; inst.color_b = b; inst.color_a = a;
  inst.is_textured = 0.0f;
  inst.clip_x0 = dr_g_clip_x0; inst.clip_y0 = dr_g_clip_y0; inst.clip_x1 = dr_g_clip_x1; inst.clip_y1 = dr_g_clip_y1;
  r_push_rect(inst);
}

internal void
dr_image(F32 x0, F32 y0, F32 x1, F32 y1, F32 u0, F32 v0, F32 u1, F32 v1, F32 r, F32 g, F32 b, F32 a)
{
  R_RectInst inst = {0};
  inst.dst_x0 = x0; inst.dst_y0 = y0; inst.dst_x1 = x1; inst.dst_y1 = y1;
  inst.src_x0 = u0; inst.src_y0 = v0; inst.src_x1 = u1; inst.src_y1 = v1;
  inst.color_r = r; inst.color_g = g; inst.color_b = b; inst.color_a = a;
  inst.is_textured = 1.0f;
  inst.clip_x0 = dr_g_clip_x0; inst.clip_y0 = dr_g_clip_y0; inst.clip_x1 = dr_g_clip_x1; inst.clip_y1 = dr_g_clip_y1;
  r_push_rect(inst);
}

internal F32
dr_text(FP_Font *font, F32 size_px, F32 x, F32 y, F32 r, F32 g, F32 b, F32 a, String8 string)
{
  local_persist FNT_Piece pieces[DR_MAX_PIECES_PER_STRING];
  F32 advance = 0.0f;
  U32 piece_count = fnt_text_pieces(font, size_px, string, x, y, pieces, ArrayCount(pieces), &advance);

  for(U32 i = 0; i < piece_count; i += 1)
  {
    FNT_Piece *p = &pieces[i];
    R_RectInst inst = {0};
    inst.dst_x0 = p->dst_x0; inst.dst_y0 = p->dst_y0; inst.dst_x1 = p->dst_x1; inst.dst_y1 = p->dst_y1;
    inst.src_x0 = p->u0; inst.src_y0 = p->v0; inst.src_x1 = p->u1; inst.src_y1 = p->v1;
    inst.color_r = r; inst.color_g = g; inst.color_b = b; inst.color_a = a;
    inst.is_textured = 1.0f;
    inst.clip_x0 = dr_g_clip_x0; inst.clip_y0 = dr_g_clip_y0; inst.clip_x1 = dr_g_clip_x1; inst.clip_y1 = dr_g_clip_y1;
    r_push_rect(inst);
  }

  return advance;
}
