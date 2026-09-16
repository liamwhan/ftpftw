// Render layer (R_ namespace): D3D11 only (no backend-switch abstraction -
// we only ever target one backend, so the hook-based indirection
// raddebugger uses to support D3D11/OpenGL/Metal would be unused
// complexity here). Everything draws as one kind of primitive: an
// instanced, optionally atlas-textured, alpha-blended rect. Both plain
// color fills (dr_rect) and glyphs (dr_text) go through the same
// instance struct and the same one draw call per frame.

#ifndef R_CORE_H
#define R_CORE_H

// COM from plain C: COBJMACROS gives the `Interface_Method(self, ...)`
// call style instead of C++ `self->Method(...)`; INITGUID + <initguid.h>
// defines the actual GUID constants inline instead of needing dxguid.lib.
// Pulled in here (rather than only in r_d3d11.c) because R_Tex2D below
// needs the D3D11 types.
#define COBJMACROS
#define INITGUID
#include <initguid.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

typedef struct R_Tex2D R_Tex2D;
struct R_Tex2D
{
  ID3D11Texture2D *texture;
  ID3D11ShaderResourceView *srv;
  U32 width;
  U32 height;
};

// One rect instance. `is_textured` selects whether the pixel shader
// samples `src_rect` out of the currently-bound atlas (a glyph) or just
// uses `color` directly (a solid fill) - see r_end_frame.
typedef struct R_RectInst R_RectInst;
struct R_RectInst
{
  F32 dst_x0, dst_y0, dst_x1, dst_y1; // pixel-space destination rect
  F32 src_x0, src_y0, src_x1, src_y1; // atlas UV rect, [0,1]
  F32 color_r, color_g, color_b, color_a;
  F32 is_textured;
};

internal void r_init(void);
internal void r_window_equip(WM_Window window);
internal void r_window_resize(U32 width, U32 height);

// Frame lifecycle: begin (clears to a color) -> any number of r_push_rect
// -> end (binds `atlas`, draws every pushed instance in one batch, presents
// with vsync).
internal void r_begin_frame(F32 clear_r, F32 clear_g, F32 clear_b, F32 clear_a);
internal void r_push_rect(R_RectInst inst);
internal void r_end_frame(R_Tex2D *atlas);

internal R_Tex2D r_tex2d_alloc(U32 width, U32 height); // R8_UNORM, alpha-only (glyph atlas)
internal void    r_tex2d_fill_region(R_Tex2D *tex, U32 x, U32 y, U32 w, U32 h, void *pixels, U32 pixels_pitch);
internal void    r_tex2d_release(R_Tex2D *tex);

#endif // R_CORE_H
