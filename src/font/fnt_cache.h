// Font cache (FNT_ namespace): owns the shared GPU glyph atlas on top of
// FP_ (shaping/rasterization) + R_ (the texture itself). Rasterized
// glyphs are packed into the atlas and cached keyed by (font, size,
// glyph-index), so the same glyph drawn in different strings reuses one
// atlas slot - shaping happens per whole string (via fp_shape, so kerning
// is correct), but atlas storage is per individual glyph.
//
// Deferred from this milestone: caching the *shaped run* itself (the
// resulting piece list) keyed by the exact string+font+size, so redrawing
// an unchanged line skips re-shaping. Not needed yet - re-shaping a short
// line every frame is trivial cost - but the atlas/glyph cache below is
// exactly what that future cache would sit on top of.

#ifndef FNT_CACHE_H
#define FNT_CACHE_H

#define FNT_ATLAS_SIZE 1024
#define FNT_MAX_GLYPHS 1024

typedef struct FNT_Piece FNT_Piece;
struct FNT_Piece
{
  F32 dst_x0, dst_y0, dst_x1, dst_y1;
  F32 u0, v0, u1, v1;
};

internal void      fnt_init(void);
internal R_Tex2D  *fnt_atlas(void);

// Shapes `string` (via FP_, so kerning is correct) and fills `out_pieces`
// (capacity `max_pieces`) with one piece per visible glyph, positioned
// starting at (pos_x, pos_y). Returns the piece count; *out_advance gets
// the run's total advance width.
internal U32 fnt_text_pieces(FP_Font *font, F32 size_px, String8 string,
                              F32 pos_x, F32 pos_y,
                              FNT_Piece *out_pieces, U32 max_pieces,
                              F32 *out_advance);

#endif // FNT_CACHE_H
