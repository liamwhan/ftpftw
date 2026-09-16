#define INITGUID
#include <initguid.h>
#include <dwrite.h>
#pragma comment(lib, "dwrite")

global IDWriteFactory *fp_g_factory;
global IDWriteTextAnalyzer *fp_g_analyzer;

internal void
fp_init(void)
{
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&fp_g_factory);
  IDWriteFactory_CreateTextAnalyzer(fp_g_factory, &fp_g_analyzer);
}

internal FP_Font
fp_font_open(String8 family_name)
{
  Temp scratch = scratch_begin(0, 0);

  int wide_count = MultiByteToWideChar(CP_UTF8, 0, (char *)family_name.str, (int)family_name.size, 0, 0);
  WCHAR *wide_name = push_array_no_zero(scratch.arena, WCHAR, wide_count + 1);
  MultiByteToWideChar(CP_UTF8, 0, (char *)family_name.str, (int)family_name.size, wide_name, wide_count);
  wide_name[wide_count] = 0;

  IDWriteFontCollection *collection = 0;
  IDWriteFactory_GetSystemFontCollection(fp_g_factory, &collection, FALSE);

  UINT32 index = 0;
  BOOL exists = FALSE;
  IDWriteFontCollection_FindFamilyName(collection, wide_name, &index, &exists);
  if(!exists)
  {
    index = 0;
  }

  IDWriteFontFamily *family = 0;
  IDWriteFontCollection_GetFontFamily(collection, index, &family);

  IDWriteFont *font_obj = 0;
  IDWriteFontFamily_GetFirstMatchingFont(family, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, &font_obj);

  FP_Font result = {0};
  IDWriteFont_CreateFontFace(font_obj, &result.face);

  IDWriteFont_Release(font_obj);
  IDWriteFontFamily_Release(family);
  IDWriteFontCollection_Release(collection);
  scratch_end(scratch);
  return result;
}

internal FP_ShapedRun
fp_shape(Arena *arena, FP_Font *font, F32 size_px, String8 string)
{
  Temp scratch = scratch_begin(&arena, 1);

  int text_len = MultiByteToWideChar(CP_UTF8, 0, (char *)string.str, (int)string.size, 0, 0);
  WCHAR *text = push_array_no_zero(scratch.arena, WCHAR, Max(1, text_len));
  if(text_len > 0)
  {
    MultiByteToWideChar(CP_UTF8, 0, (char *)string.str, (int)string.size, text, text_len);
  }

  FP_ShapedRun result = {0};
  if(text_len > 0)
  {
    DWRITE_SCRIPT_ANALYSIS script = {0, DWRITE_SCRIPT_SHAPES_DEFAULT};

    UINT16 *cluster_map = push_array_no_zero(scratch.arena, UINT16, text_len);
    DWRITE_SHAPING_TEXT_PROPERTIES *text_props = push_array_no_zero(scratch.arena, DWRITE_SHAPING_TEXT_PROPERTIES, text_len);
    UINT32 max_glyphs = (UINT32)(text_len * 3 + 16);
    UINT16 *glyph_indices = push_array_no_zero(scratch.arena, UINT16, max_glyphs);
    DWRITE_SHAPING_GLYPH_PROPERTIES *glyph_props = push_array_no_zero(scratch.arena, DWRITE_SHAPING_GLYPH_PROPERTIES, max_glyphs);
    UINT32 actual_glyph_count = 0;

    HRESULT hr = IDWriteTextAnalyzer_GetGlyphs(fp_g_analyzer, text, (UINT32)text_len, font->face, FALSE, FALSE,
                                                &script, 0, 0, 0, 0, 0, max_glyphs,
                                                cluster_map, text_props, glyph_indices, glyph_props,
                                                &actual_glyph_count);
    if(SUCCEEDED(hr) && actual_glyph_count > 0)
    {
      FLOAT *glyph_advances = push_array_no_zero(scratch.arena, FLOAT, actual_glyph_count);
      DWRITE_GLYPH_OFFSET *glyph_offsets = push_array_no_zero(scratch.arena, DWRITE_GLYPH_OFFSET, actual_glyph_count);

      IDWriteTextAnalyzer_GetGlyphPlacements(fp_g_analyzer, text, cluster_map, text_props, (UINT32)text_len,
                                              glyph_indices, glyph_props, actual_glyph_count, font->face,
                                              size_px, FALSE, FALSE, &script, 0, 0, 0, 0,
                                              glyph_advances, glyph_offsets);

      result.glyphs = push_array_no_zero(arena, FP_ShapedGlyph, actual_glyph_count);
      result.glyph_count = actual_glyph_count;
      F32 pen_x = 0.0f;
      for(U32 i = 0; i < actual_glyph_count; i += 1)
      {
        result.glyphs[i].glyph_id = glyph_indices[i];
        result.glyphs[i].pen_x = pen_x + glyph_offsets[i].advanceOffset;
        result.glyphs[i].pen_y = -glyph_offsets[i].ascenderOffset;
        pen_x += glyph_advances[i];
      }
      result.advance = pen_x;
    }
  }

  scratch_end(scratch);
  return result;
}

internal FP_Bitmap
fp_rasterize_glyph(Arena *arena, FP_Font *font, F32 size_px, U32 glyph_id)
{
  FP_Bitmap result = {0};

  UINT16 glyph_index = (UINT16)glyph_id;
  FLOAT advance = 0.0f;
  DWRITE_GLYPH_OFFSET offset = {0};
  DWRITE_GLYPH_RUN run = {0};
  run.fontFace = font->face;
  run.fontEmSize = size_px;
  run.glyphCount = 1;
  run.glyphIndices = &glyph_index;
  run.glyphAdvances = &advance;
  run.glyphOffsets = &offset;

  IDWriteGlyphRunAnalysis *analysis = 0;
  HRESULT hr = IDWriteFactory_CreateGlyphRunAnalysis(fp_g_factory, &run, 1.0f, 0,
                                                      DWRITE_RENDERING_MODE_NATURAL,
                                                      DWRITE_MEASURING_MODE_NATURAL,
                                                      0.0f, 0.0f, &analysis);
  if(SUCCEEDED(hr))
  {
    RECT bounds = {0};
    IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(analysis, DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds);
    S32 width = bounds.right - bounds.left;
    S32 height = bounds.bottom - bounds.top;
    if(width > 0 && height > 0)
    {
      Temp scratch = scratch_begin(&arena, 1);
      U32 rgb_size = (U32)(width * height * 3);
      U8 *rgb = push_array_no_zero(scratch.arena, U8, rgb_size);
      hr = IDWriteGlyphRunAnalysis_CreateAlphaTexture(analysis, DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, rgb, rgb_size);
      if(SUCCEEDED(hr))
      {
        result.width = (U32)width;
        result.height = (U32)height;
        result.offset_x = bounds.left;
        result.offset_y = bounds.top;
        result.pixels = push_array_no_zero(arena, U8, (U64)width * (U64)height);
        for(S32 i = 0; i < width * height; i += 1)
        {
          U32 r = rgb[i * 3 + 0], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
          result.pixels[i] = (U8)((r + g + b) / 3);
        }
      }
      scratch_end(scratch);
    }
    IDWriteGlyphRunAnalysis_Release(analysis);
  }

  return result;
}
