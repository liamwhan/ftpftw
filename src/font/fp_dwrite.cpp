// See fp_dwrite.h for why this one file is C++ and compiled/linked
// separately from the rest of the (plain C, unity-build) codebase.
// Deliberately does not use this project's arena/String8-function/TCTX
// helpers - those are file-static to the main translation unit.

#include "../base/base_context_cracking.h"
#include "../base/base_core.h"
#include "../base/base_system.h"
#include "../base/base_memory.h"
#include "../base/base_arena.h"
#include "../base/base_strings.h"

#include <dwrite.h>
#pragma comment(lib, "dwrite")

#include "fp_dwrite.h"

#define FP_MAX_SHAPE_LEN 2048

static IDWriteFactory *fp_g_factory;
static IDWriteTextAnalyzer *fp_g_analyzer;

void
fp_init(void)
{
  // dwrite.h's interfaces have no separate IID_* globals (unlike D3D/DXGI) -
  // just a DECLSPEC_UUID attribute, so __uuidof is the only way to get the
  // IID here (MSVC-native; would need -fms-extensions under clang).
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&fp_g_factory);
  fp_g_factory->CreateTextAnalyzer(&fp_g_analyzer);
}

FP_Font
fp_font_open(char *family_name_utf8)
{
  WCHAR wide_name[256];
  int wide_count = MultiByteToWideChar(CP_UTF8, 0, family_name_utf8, -1, wide_name, ArrayCount(wide_name));
  if(wide_count == 0)
  {
    wide_name[0] = 0;
  }

  IDWriteFontCollection *collection = 0;
  fp_g_factory->GetSystemFontCollection(&collection, FALSE);

  UINT32 index = 0;
  BOOL exists = FALSE;
  collection->FindFamilyName(wide_name, &index, &exists);
  if(!exists)
  {
    index = 0;
  }

  IDWriteFontFamily *family = 0;
  collection->GetFontFamily(index, &family);

  IDWriteFont *font_obj = 0;
  family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                DWRITE_FONT_STYLE_NORMAL, &font_obj);

  FP_Font result = {0};
  IDWriteFontFace *face = 0;
  font_obj->CreateFontFace(&face);
  result.face = face;

  font_obj->Release();
  family->Release();
  collection->Release();
  return result;
}

U32
fp_shape(FP_Font *font, F32 size_px, U8 *string_utf8, U64 string_len,
          FP_ShapedGlyph *out_glyphs, U32 max_glyphs, F32 *out_advance)
{
  U32 result = 0;
  if(out_advance != 0)
  {
    *out_advance = 0.0f;
  }

  int text_len = MultiByteToWideChar(CP_UTF8, 0, (char *)string_utf8, (int)string_len, 0, 0);
  if(text_len <= 0)
  {
    return 0;
  }
  if(text_len > FP_MAX_SHAPE_LEN)
  {
    text_len = FP_MAX_SHAPE_LEN;
  }

  WCHAR text[FP_MAX_SHAPE_LEN];
  MultiByteToWideChar(CP_UTF8, 0, (char *)string_utf8, (int)string_len, text, text_len);

  IDWriteFontFace *face = (IDWriteFontFace *)font->face;
  DWRITE_SCRIPT_ANALYSIS script = {0, DWRITE_SCRIPT_SHAPES_DEFAULT};

  UINT16 cluster_map[FP_MAX_SHAPE_LEN];
  DWRITE_SHAPING_TEXT_PROPERTIES text_props[FP_MAX_SHAPE_LEN];
  UINT32 max_dwrite_glyphs = FP_MAX_SHAPE_LEN * 3;
  static UINT16 glyph_indices[FP_MAX_SHAPE_LEN * 3];
  static DWRITE_SHAPING_GLYPH_PROPERTIES glyph_props[FP_MAX_SHAPE_LEN * 3];
  UINT32 actual_glyph_count = 0;

  HRESULT hr = fp_g_analyzer->GetGlyphs(text, (UINT32)text_len, face, FALSE, FALSE,
                                         &script, 0, 0, 0, 0, 0, max_dwrite_glyphs,
                                         cluster_map, text_props, glyph_indices, glyph_props,
                                         &actual_glyph_count);
  if(SUCCEEDED(hr) && actual_glyph_count > 0)
  {
    static FLOAT glyph_advances[FP_MAX_SHAPE_LEN * 3];
    static DWRITE_GLYPH_OFFSET glyph_offsets[FP_MAX_SHAPE_LEN * 3];

    fp_g_analyzer->GetGlyphPlacements(text, cluster_map, text_props, (UINT32)text_len,
                                       glyph_indices, glyph_props, actual_glyph_count, face,
                                       size_px, FALSE, FALSE, &script, 0, 0, 0, 0,
                                       glyph_advances, glyph_offsets);

    F32 pen_x = 0.0f;
    for(UINT32 i = 0; i < actual_glyph_count; i += 1)
    {
      if(i < max_glyphs)
      {
        out_glyphs[i].glyph_id = glyph_indices[i];
        out_glyphs[i].pen_x = pen_x + glyph_offsets[i].advanceOffset;
        out_glyphs[i].pen_y = -glyph_offsets[i].ascenderOffset;
      }
      pen_x += glyph_advances[i];
    }
    result = actual_glyph_count;
    if(out_advance != 0)
    {
      *out_advance = pen_x;
    }
  }

  return result;
}

B32
fp_rasterize_glyph(FP_Font *font, F32 size_px, U32 glyph_id,
                     U8 *out_pixels, U32 out_pixels_capacity,
                     U32 *out_width, U32 *out_height,
                     S32 *out_offset_x, S32 *out_offset_y)
{
  *out_width = 0;
  *out_height = 0;
  *out_offset_x = 0;
  *out_offset_y = 0;
  B32 result = 0;

  IDWriteFontFace *face = (IDWriteFontFace *)font->face;

  UINT16 glyph_index = (UINT16)glyph_id;
  FLOAT advance = 0.0f;
  DWRITE_GLYPH_OFFSET offset = {0};
  DWRITE_GLYPH_RUN run = {0};
  run.fontFace = face;
  run.fontEmSize = size_px;
  run.glyphCount = 1;
  run.glyphIndices = &glyph_index;
  run.glyphAdvances = &advance;
  run.glyphOffsets = &offset;

  IDWriteGlyphRunAnalysis *analysis = 0;
  HRESULT hr = fp_g_factory->CreateGlyphRunAnalysis(&run, 1.0f, 0,
                                                      DWRITE_RENDERING_MODE_NATURAL,
                                                      DWRITE_MEASURING_MODE_NATURAL,
                                                      0.0f, 0.0f, &analysis);
  if(SUCCEEDED(hr))
  {
    RECT bounds = {0};
    analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds);
    S32 width = bounds.right - bounds.left;
    S32 height = bounds.bottom - bounds.top;
    if(width > 0 && height > 0 && (U32)(width * height) <= out_pixels_capacity)
    {
      U32 rgb_size = (U32)(width * height * 3);
      static U8 rgb[1024 * 1024 * 3]; // generous fixed scratch - big enough for any glyph we'll ever render
      if(rgb_size <= sizeof(rgb))
      {
        hr = analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, rgb, rgb_size);
        if(SUCCEEDED(hr))
        {
          *out_width = (U32)width;
          *out_height = (U32)height;
          *out_offset_x = bounds.left;
          *out_offset_y = bounds.top;
          for(S32 i = 0; i < width * height; i += 1)
          {
            U32 r = rgb[i * 3 + 0], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
            out_pixels[i] = (U8)((r + g + b) / 3);
          }
          result = 1;
        }
      }
    }
    analysis->Release();
  }

  return result;
}
