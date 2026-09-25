// Font provider (FP_ namespace): DirectWrite wrapper. Shapes a whole
// string at once via IDWriteTextAnalyzer (proper kerning, not a per-glyph
// advance loop - mirrors raddebugger's whole-run shaping), and rasterizes
// individual glyphs on request so fnt_cache can atlas-pack and reuse them
// across strings.
//
// The script analysis passed to the analyzer is hardcoded to "default
// Latin, LTR" rather than run through DirectWrite's script itemizer
// (which needs a custom IDWriteTextAnalysisSource callback object) -
// correct for the plain ASCII/Latin filenames and log text this client
// actually renders; a real script-itemization pass would only matter for
// mixed-script or RTL text, which isn't a case we have.
//
// This is a plain-C-callable boundary, but it's implemented in
// fp_dwrite.cpp (compiled separately, as C++) rather than in this
// project's usual single unity-build .c - <dwrite.h> itself is not
// C-includable (DWRITE_MAKE_OPENTYPE_TAG uses static_cast unconditionally,
// even inside a plain C-visible enum - an actual bug/limitation in the SDK
// header, not something we can route around from our side other than
// compiling that one file as C++). So FP_Font.face is kept an opaque
// void* here (the real type, IDWriteFontFace*, is only known inside
// fp_dwrite.cpp), and every function below takes/fills caller-owned
// buffers instead of allocating - fp_dwrite.cpp deliberately doesn't
// touch this project's arena/String8-function/TCTX machinery, since
// those are `internal` (file-static) to the main unity-build translation
// unit and not linkable from a separate one.

#ifndef FP_DWRITE_H
#define FP_DWRITE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FP_Font FP_Font;
struct FP_Font
{
  void *face; // IDWriteFontFace*, opaque here - see note above
};

typedef struct FP_ShapedGlyph FP_ShapedGlyph;
struct FP_ShapedGlyph
{
  U32 glyph_id;
  F32 pen_x; // glyph origin, relative to the run's start (baseline)
  F32 pen_y; // +down
};

void fp_init(void);

// `family_name` must be null-terminated (e.g. from str8_to_cstring) -
// this file can't call our str8_ helpers itself (see note above).
FP_Font fp_font_open(char *family_name_utf8);

// Shapes `string_utf8` (byte length `string_len`) as one run. Writes up
// to `max_glyphs` glyphs into `out_glyphs`; returns the actual glyph
// count (may exceed max_glyphs - only the first max_glyphs are written).
// *out_advance gets the run's total advance width.
U32 fp_shape(FP_Font *font, F32 size_px, U8 *string_utf8, U64 string_len,
             FP_ShapedGlyph *out_glyphs, U32 max_glyphs, F32 *out_advance);

// Rasterizes one glyph's alpha (grayscale AA) bitmap into `out_pixels`
// (capacity `out_pixels_capacity` bytes, tightly packed rows). Returns 0
// (and writes zeroed metrics) if the glyph has no ink (e.g. space) or the
// bitmap doesn't fit the caller's buffer.
B32 fp_rasterize_glyph(FP_Font *font, F32 size_px, U32 glyph_id,
                        U8 *out_pixels, U32 out_pixels_capacity,
                        U32 *out_width, U32 *out_height,
                        S32 *out_offset_x, S32 *out_offset_y);

#ifdef __cplusplus
}
#endif

#endif // FP_DWRITE_H
