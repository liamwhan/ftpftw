#define FNT_MAX_GLYPHS_PER_SHAPE 1024
#define FNT_MAX_GLYPH_BITMAP_DIM 128

typedef struct FNT_GlyphEntry FNT_GlyphEntry;
struct FNT_GlyphEntry
{
  void *face; // FP_Font.face identity (opaque - see fp_dwrite.h)
  F32 size_px;
  U32 glyph_id;
  F32 u0, v0, u1, v1;
  S32 offset_x, offset_y;
  U32 width, height;
};

global struct
{
  R_Tex2D atlas;
  U32 shelf_x, shelf_y, shelf_h;
  FNT_GlyphEntry glyphs[FNT_MAX_GLYPHS];
  U32 glyph_count;
}
fnt_g;

internal void
fnt_init(void)
{
  MemoryZeroStruct(&fnt_g);
  fnt_g.atlas = r_tex2d_alloc(FNT_ATLAS_SIZE, FNT_ATLAS_SIZE);
}

internal R_Tex2D *
fnt_atlas(void)
{
  return &fnt_g.atlas;
}

internal FNT_GlyphEntry *
fnt_glyph_find(void *face, F32 size_px, U32 glyph_id)
{
  FNT_GlyphEntry *result = 0;
  for(U32 i = 0; i < fnt_g.glyph_count; i += 1)
  {
    FNT_GlyphEntry *e = &fnt_g.glyphs[i];
    if(e->face == face && e->size_px == size_px && e->glyph_id == glyph_id)
    {
      result = e;
      break;
    }
  }
  return result;
}

// Shared shelf/row packer: used both for glyphs (below) and for arbitrary
// bitmaps (fnt_atlas_pack_bitmap, e.g. procedurally-drawn UI icons) so both
// kinds of quad sample the same atlas texture and still batch into one
// draw call.
internal void
fnt__atlas_pack(U32 width, U32 height, U8 *pixels, U32 pixels_pitch,
                F32 *out_u0, F32 *out_v0, F32 *out_u1, F32 *out_v1)
{
  // 1px gutter around each packed rect so linear sampling never bleeds
  // into a neighboring rect's texels.
  U32 packed_w = width + 1;
  U32 packed_h = height + 1;
  if(fnt_g.shelf_x + packed_w > FNT_ATLAS_SIZE)
  {
    fnt_g.shelf_x = 0;
    fnt_g.shelf_y += fnt_g.shelf_h;
    fnt_g.shelf_h = 0;
  }
  AssertAlways(fnt_g.shelf_y + packed_h <= FNT_ATLAS_SIZE); // atlas out of room

  U32 px = fnt_g.shelf_x;
  U32 py = fnt_g.shelf_y;
  r_tex2d_fill_region(&fnt_g.atlas, px, py, width, height, pixels, pixels_pitch);

  *out_u0 = (F32)px / (F32)FNT_ATLAS_SIZE;
  *out_v0 = (F32)py / (F32)FNT_ATLAS_SIZE;
  *out_u1 = (F32)(px + width) / (F32)FNT_ATLAS_SIZE;
  *out_v1 = (F32)(py + height) / (F32)FNT_ATLAS_SIZE;

  fnt_g.shelf_x += packed_w;
  fnt_g.shelf_h = Max(fnt_g.shelf_h, packed_h);
}

internal void
fnt_atlas_pack_bitmap(U8 *pixels, U32 w, U32 h, F32 *out_u0, F32 *out_v0, F32 *out_u1, F32 *out_v1)
{
  fnt__atlas_pack(w, h, pixels, w, out_u0, out_v0, out_u1, out_v1);
}

internal FNT_GlyphEntry *
fnt_glyph_insert(FP_Font *font, F32 size_px, U32 glyph_id)
{
  local_persist U8 pixel_buf[FNT_MAX_GLYPH_BITMAP_DIM * FNT_MAX_GLYPH_BITMAP_DIM];
  U32 width = 0, height = 0;
  S32 offset_x = 0, offset_y = 0;
  B32 has_ink = fp_rasterize_glyph(font, size_px, glyph_id, pixel_buf, sizeof(pixel_buf),
                                     &width, &height, &offset_x, &offset_y);

  FNT_GlyphEntry *e = 0;
  if(fnt_g.glyph_count < FNT_MAX_GLYPHS)
  {
    e = &fnt_g.glyphs[fnt_g.glyph_count];
    fnt_g.glyph_count += 1;
    e->face = font->face;
    e->size_px = size_px;
    e->glyph_id = glyph_id;
    e->offset_x = offset_x;
    e->offset_y = offset_y;
    e->width = width;
    e->height = height;

    if(has_ink && width > 0 && height > 0)
    {
      fnt__atlas_pack(width, height, pixel_buf, width, &e->u0, &e->v0, &e->u1, &e->v1);
    }
  }

  return e;
}

internal U32
fnt_text_pieces(FP_Font *font, F32 size_px, String8 string,
                 F32 pos_x, F32 pos_y,
                 FNT_Piece *out_pieces, U32 max_pieces,
                 F32 *out_advance)
{
  local_persist FP_ShapedGlyph shaped[FNT_MAX_GLYPHS_PER_SHAPE];
  U32 shaped_count = fp_shape(font, size_px, string.str, string.size, shaped, ArrayCount(shaped), out_advance);
  shaped_count = Min(shaped_count, ArrayCount(shaped));

  U32 piece_count = 0;
  for(U32 i = 0; i < shaped_count; i += 1)
  {
    FP_ShapedGlyph *g = &shaped[i];
    FNT_GlyphEntry *e = fnt_glyph_find(font->face, size_px, g->glyph_id);
    if(e == 0)
    {
      e = fnt_glyph_insert(font, size_px, g->glyph_id);
    }
    if(e != 0 && e->width > 0 && e->height > 0 && piece_count < max_pieces)
    {
      FNT_Piece *piece = &out_pieces[piece_count];
      piece->dst_x0 = pos_x + g->pen_x + (F32)e->offset_x;
      piece->dst_y0 = pos_y + g->pen_y + (F32)e->offset_y;
      piece->dst_x1 = piece->dst_x0 + (F32)e->width;
      piece->dst_y1 = piece->dst_y0 + (F32)e->height;
      piece->u0 = e->u0;
      piece->v0 = e->v0;
      piece->u1 = e->u1;
      piece->v1 = e->v1;
      piece_count += 1;
    }
  }

  return piece_count;
}
