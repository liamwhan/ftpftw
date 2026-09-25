// Draw layer (DR_ namespace): the only API main.c (and later a file-list
// view) should need. Hides R_/FP_/FNT_ details - callers just say what to
// draw, not how it gets batched or atlas-packed.

#ifndef DR_CORE_H
#define DR_CORE_H

internal void dr_begin_frame(F32 clear_r, F32 clear_g, F32 clear_b, F32 clear_a);
internal void dr_end_frame(void);

// Every dr_rect/dr_image/dr_text call after dr_set_clip is pixel-clipped to
// that rect (tested in the pixel shader against SV_Position - see
// R_RectInst.clip_* / r_d3d11.c) until the next dr_set_clip or
// dr_clear_clip. Not a stack - callers set/clear around whatever pane
// they're drawing, same flat style as everything else here. Reset to "no
// clipping" at the start of every frame by dr_begin_frame.
internal void dr_set_clip(F32 x0, F32 y0, F32 x1, F32 y1);
internal void dr_clear_clip(void);

internal void dr_rect(F32 x0, F32 y0, F32 x1, F32 y1, F32 r, F32 g, F32 b, F32 a);

// A textured rect at a caller-given atlas UV rect (vs dr_text's automatic
// per-glyph UV lookup) - what draws a UI icon once it's packed into the
// atlas (see fnt_atlas_pack_bitmap / src/ui/ui_icons.h).
internal void dr_image(F32 x0, F32 y0, F32 x1, F32 y1, F32 u0, F32 v0, F32 u1, F32 v1, F32 r, F32 g, F32 b, F32 a);

// Returns the drawn string's advance width (pixels), so callers can lay
// out subsequent text without needing their own font metrics.
internal F32 dr_text(FP_Font *font, F32 size_px, F32 x, F32 y, F32 r, F32 g, F32 b, F32 a, String8 string);

#endif // DR_CORE_H
