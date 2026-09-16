// D3D11 backend for the render layer. One instanced-quad pipeline: a
// static 4-vertex unit quad (triangle strip) plus a dynamic per-instance
// buffer (R_RectInst) rewritten via Map/Discard once per frame. Both
// solid-color rects and atlas-sampled glyphs are the same instance/draw
// call - the pixel shader just checks `is_textured`.
//
// COBJMACROS/INITGUID/<d3d11.h> etc. are pulled in by r_core.h (R_Tex2D
// needs the types), included before this file in main.c's unity build.

#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "d3dcompiler")

#define R_MAX_INSTANCES 16384

global struct
{
  ID3D11Device *device;
  ID3D11DeviceContext *ctx;
  IDXGISwapChain *swapchain;
  ID3D11RenderTargetView *rtv;

  ID3D11Buffer *vbuf_quad;
  ID3D11Buffer *vbuf_inst;
  ID3D11Buffer *cbuf_viewport;
  ID3D11InputLayout *input_layout;
  ID3D11VertexShader *vs;
  ID3D11PixelShader *ps;
  ID3D11SamplerState *sampler;
  ID3D11BlendState *blend_state;
  ID3D11RasterizerState *rasterizer_state;

  U32 window_width;
  U32 window_height;

  R_RectInst instances[R_MAX_INSTANCES];
  U64 instance_count;
}
r_g;

#define R_D3D_CHECK(hr) AssertAlways(SUCCEEDED(hr))

global char *r_g_vs_src =
"cbuffer Constants : register(b0) { float2 viewport_size; };\n"
"struct VS_INPUT {\n"
"  float2 unit_pos : UNIT_POS;\n"
"  float4 dst_rect : DST_RECT;\n"
"  float4 src_rect : SRC_RECT;\n"
"  float4 color : COLOR;\n"
"  float is_textured : TEXTURED;\n"
"  float4 clip_rect : CLIP_RECT;\n"
"};\n"
"struct VS_OUTPUT {\n"
"  float4 pos : SV_Position;\n"
"  float2 uv : TEXCOORD0;\n"
"  float4 color : COLOR0;\n"
"  float is_textured : TEXTURED0;\n"
"  float4 clip_rect : CLIP_RECT0;\n"
"};\n"
"VS_OUTPUT vs_main(VS_INPUT input) {\n"
"  float2 pixel_pos = lerp(input.dst_rect.xy, input.dst_rect.zw, input.unit_pos);\n"
"  float2 ndc = (pixel_pos / viewport_size) * float2(2, -2) + float2(-1, 1);\n"
"  VS_OUTPUT o;\n"
"  o.pos = float4(ndc, 0, 1);\n"
"  o.uv = lerp(input.src_rect.xy, input.src_rect.zw, input.unit_pos);\n"
"  o.color = input.color;\n"
"  o.is_textured = input.is_textured;\n"
"  o.clip_rect = input.clip_rect;\n"
"  return o;\n"
"}\n";

global char *r_g_ps_src =
"Texture2D atlas_tex : register(t0);\n"
"SamplerState atlas_sampler : register(s0);\n"
"struct VS_OUTPUT {\n"
"  float4 pos : SV_Position;\n"
"  float2 uv : TEXCOORD0;\n"
"  float4 color : COLOR0;\n"
"  float is_textured : TEXTURED0;\n"
"  float4 clip_rect : CLIP_RECT0;\n"
"};\n"
"float4 ps_main(VS_OUTPUT input) : SV_Target {\n"
"  if(input.pos.x < input.clip_rect.x || input.pos.x > input.clip_rect.z ||\n"
"     input.pos.y < input.clip_rect.y || input.pos.y > input.clip_rect.w) {\n"
"    discard;\n"
"  }\n"
"  float4 color = input.color;\n"
"  if(input.is_textured > 0.5) {\n"
"    float a = atlas_tex.Sample(atlas_sampler, input.uv).r;\n"
"    color.a *= a;\n"
"  }\n"
"  return color;\n"
"}\n";

internal void
r_init(void)
{
  MemoryZeroStruct(&r_g);
}

internal void
r_window_equip(WM_Window window)
{
  HWND hwnd = (HWND)wm_native_handle(window);
  U32 width, height;
  wm_client_size(window, &width, &height);
  r_g.window_width = width;
  r_g.window_height = height;

  DXGI_SWAP_CHAIN_DESC sc_desc = {0};
  sc_desc.BufferDesc.Width = width;
  sc_desc.BufferDesc.Height = height;
  sc_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sc_desc.SampleDesc.Count = 1;
  sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sc_desc.BufferCount = 2;
  sc_desc.OutputWindow = hwnd;
  sc_desc.Windowed = TRUE;
  sc_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(0, D3D_DRIVER_TYPE_HARDWARE, 0, 0,
                                              &feature_level, 1, D3D11_SDK_VERSION,
                                              &sc_desc, &r_g.swapchain, &r_g.device,
                                              0, &r_g.ctx);
  R_D3D_CHECK(hr);

  ID3D11Texture2D *backbuffer = 0;
  hr = IDXGISwapChain_GetBuffer(r_g.swapchain, 0, &IID_ID3D11Texture2D, (void **)&backbuffer);
  R_D3D_CHECK(hr);
  hr = ID3D11Device_CreateRenderTargetView(r_g.device, (ID3D11Resource *)backbuffer, 0, &r_g.rtv);
  R_D3D_CHECK(hr);
  ID3D11Texture2D_Release(backbuffer);

  ID3DBlob *vs_blob = 0, *ps_blob = 0, *err_blob = 0;
  hr = D3DCompile(r_g_vs_src, strlen(r_g_vs_src), 0, 0, 0, "vs_main", "vs_5_0", 0, 0, &vs_blob, &err_blob);
  if(FAILED(hr) && err_blob != 0)
  {
    fprintf(stderr, "vs compile error: %s\n", (char *)ID3D10Blob_GetBufferPointer(err_blob));
  }
  R_D3D_CHECK(hr);
  hr = D3DCompile(r_g_ps_src, strlen(r_g_ps_src), 0, 0, 0, "ps_main", "ps_5_0", 0, 0, &ps_blob, &err_blob);
  if(FAILED(hr) && err_blob != 0)
  {
    fprintf(stderr, "ps compile error: %s\n", (char *)ID3D10Blob_GetBufferPointer(err_blob));
  }
  R_D3D_CHECK(hr);

  hr = ID3D11Device_CreateVertexShader(r_g.device, ID3D10Blob_GetBufferPointer(vs_blob),
                                        ID3D10Blob_GetBufferSize(vs_blob), 0, &r_g.vs);
  R_D3D_CHECK(hr);
  hr = ID3D11Device_CreatePixelShader(r_g.device, ID3D10Blob_GetBufferPointer(ps_blob),
                                       ID3D10Blob_GetBufferSize(ps_blob), 0, &r_g.ps);
  R_D3D_CHECK(hr);

  D3D11_INPUT_ELEMENT_DESC layout[] =
  {
    {"UNIT_POS", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA,   0},
    {"DST_RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,  D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"SRC_RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"TEXTURED", 0, DXGI_FORMAT_R32_FLOAT,          1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    {"CLIP_RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 52, D3D11_INPUT_PER_INSTANCE_DATA, 1},
  };
  hr = ID3D11Device_CreateInputLayout(r_g.device, layout, ArrayCount(layout),
                                       ID3D10Blob_GetBufferPointer(vs_blob),
                                       ID3D10Blob_GetBufferSize(vs_blob), &r_g.input_layout);
  R_D3D_CHECK(hr);
  ID3D10Blob_Release(vs_blob);
  ID3D10Blob_Release(ps_blob);

  F32 quad_verts[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
  D3D11_BUFFER_DESC quad_desc = {0};
  quad_desc.ByteWidth = sizeof(quad_verts);
  quad_desc.Usage = D3D11_USAGE_IMMUTABLE;
  quad_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  D3D11_SUBRESOURCE_DATA quad_data = {quad_verts, 0, 0};
  hr = ID3D11Device_CreateBuffer(r_g.device, &quad_desc, &quad_data, &r_g.vbuf_quad);
  R_D3D_CHECK(hr);

  D3D11_BUFFER_DESC inst_desc = {0};
  inst_desc.ByteWidth = sizeof(R_RectInst) * R_MAX_INSTANCES;
  inst_desc.Usage = D3D11_USAGE_DYNAMIC;
  inst_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  inst_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = ID3D11Device_CreateBuffer(r_g.device, &inst_desc, 0, &r_g.vbuf_inst);
  R_D3D_CHECK(hr);

  D3D11_BUFFER_DESC cbuf_desc = {0};
  cbuf_desc.ByteWidth = 16;
  cbuf_desc.Usage = D3D11_USAGE_DYNAMIC;
  cbuf_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  cbuf_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = ID3D11Device_CreateBuffer(r_g.device, &cbuf_desc, 0, &r_g.cbuf_viewport);
  R_D3D_CHECK(hr);

  D3D11_SAMPLER_DESC samp_desc = {0};
  samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
  hr = ID3D11Device_CreateSamplerState(r_g.device, &samp_desc, &r_g.sampler);
  R_D3D_CHECK(hr);

  D3D11_BLEND_DESC blend_desc = {0};
  blend_desc.RenderTarget[0].BlendEnable = TRUE;
  blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  hr = ID3D11Device_CreateBlendState(r_g.device, &blend_desc, &r_g.blend_state);
  R_D3D_CHECK(hr);

  D3D11_RASTERIZER_DESC rast_desc = {0};
  rast_desc.FillMode = D3D11_FILL_SOLID;
  rast_desc.CullMode = D3D11_CULL_NONE;
  hr = ID3D11Device_CreateRasterizerState(r_g.device, &rast_desc, &r_g.rasterizer_state);
  R_D3D_CHECK(hr);
}

internal void
r_window_resize(U32 width, U32 height)
{
  if(width == 0 || height == 0 || r_g.swapchain == 0)
  {
    return;
  }
  r_g.window_width = width;
  r_g.window_height = height;

  ID3D11DeviceContext_OMSetRenderTargets(r_g.ctx, 0, 0, 0);
  ID3D11RenderTargetView_Release(r_g.rtv);
  r_g.rtv = 0;

  IDXGISwapChain_ResizeBuffers(r_g.swapchain, 0, width, height, DXGI_FORMAT_UNKNOWN, 0);

  ID3D11Texture2D *backbuffer = 0;
  IDXGISwapChain_GetBuffer(r_g.swapchain, 0, &IID_ID3D11Texture2D, (void **)&backbuffer);
  ID3D11Device_CreateRenderTargetView(r_g.device, (ID3D11Resource *)backbuffer, 0, &r_g.rtv);
  ID3D11Texture2D_Release(backbuffer);
}

internal void
r_begin_frame(F32 clear_r, F32 clear_g, F32 clear_b, F32 clear_a)
{
  r_g.instance_count = 0;

  F32 clear_color[4] = {clear_r, clear_g, clear_b, clear_a};
  ID3D11DeviceContext_ClearRenderTargetView(r_g.ctx, r_g.rtv, clear_color);
}

internal void
r_push_rect(R_RectInst inst)
{
  if(r_g.instance_count < R_MAX_INSTANCES)
  {
    r_g.instances[r_g.instance_count] = inst;
    r_g.instance_count += 1;
  }
}

internal void
r_end_frame(R_Tex2D *atlas)
{
  D3D11_MAPPED_SUBRESOURCE mapped = {0};

  ID3D11DeviceContext_Map(r_g.ctx, (ID3D11Resource *)r_g.cbuf_viewport, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
  F32 viewport_size[2] = {(F32)r_g.window_width, (F32)r_g.window_height};
  MemoryCopy(mapped.pData, viewport_size, sizeof(viewport_size));
  ID3D11DeviceContext_Unmap(r_g.ctx, (ID3D11Resource *)r_g.cbuf_viewport, 0);

  if(r_g.instance_count > 0)
  {
    ID3D11DeviceContext_Map(r_g.ctx, (ID3D11Resource *)r_g.vbuf_inst, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    MemoryCopy(mapped.pData, r_g.instances, sizeof(R_RectInst) * r_g.instance_count);
    ID3D11DeviceContext_Unmap(r_g.ctx, (ID3D11Resource *)r_g.vbuf_inst, 0);

    D3D11_VIEWPORT viewport = {0, 0, (F32)r_g.window_width, (F32)r_g.window_height, 0, 1};
    ID3D11DeviceContext_RSSetViewports(r_g.ctx, 1, &viewport);
    ID3D11DeviceContext_RSSetState(r_g.ctx, r_g.rasterizer_state);
    ID3D11DeviceContext_OMSetRenderTargets(r_g.ctx, 1, &r_g.rtv, 0);
    F32 blend_factor[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_OMSetBlendState(r_g.ctx, r_g.blend_state, blend_factor, 0xffffffff);

    ID3D11DeviceContext_IASetPrimitiveTopology(r_g.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetInputLayout(r_g.ctx, r_g.input_layout);
    UINT strides[2] = {sizeof(F32) * 2, sizeof(R_RectInst)};
    UINT offsets[2] = {0, 0};
    ID3D11Buffer *buffers[2] = {r_g.vbuf_quad, r_g.vbuf_inst};
    ID3D11DeviceContext_IASetVertexBuffers(r_g.ctx, 0, 2, buffers, strides, offsets);

    ID3D11DeviceContext_VSSetShader(r_g.ctx, r_g.vs, 0, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(r_g.ctx, 0, 1, &r_g.cbuf_viewport);
    ID3D11DeviceContext_PSSetShader(r_g.ctx, r_g.ps, 0, 0);
    ID3D11DeviceContext_PSSetSamplers(r_g.ctx, 0, 1, &r_g.sampler);
    if(atlas != 0 && atlas->srv != 0)
    {
      ID3D11DeviceContext_PSSetShaderResources(r_g.ctx, 0, 1, &atlas->srv);
    }

    ID3D11DeviceContext_DrawInstanced(r_g.ctx, 4, (UINT)r_g.instance_count, 0, 0);
  }

  IDXGISwapChain_Present(r_g.swapchain, 1, 0);
}

internal R_Tex2D
r_tex2d_alloc(U32 width, U32 height)
{
  R_Tex2D result = {0};
  result.width = width;
  result.height = height;

  D3D11_TEXTURE2D_DESC desc = {0};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr = ID3D11Device_CreateTexture2D(r_g.device, &desc, 0, &result.texture);
  R_D3D_CHECK(hr);
  hr = ID3D11Device_CreateShaderResourceView(r_g.device, (ID3D11Resource *)result.texture, 0, &result.srv);
  R_D3D_CHECK(hr);

  return result;
}

internal void
r_tex2d_fill_region(R_Tex2D *tex, U32 x, U32 y, U32 w, U32 h, void *pixels, U32 pixels_pitch)
{
  D3D11_BOX box = {x, y, 0, x + w, y + h, 1};
  ID3D11DeviceContext_UpdateSubresource(r_g.ctx, (ID3D11Resource *)tex->texture, 0, &box, pixels, pixels_pitch, 0);
}

internal void
r_tex2d_release(R_Tex2D *tex)
{
  ID3D11ShaderResourceView_Release(tex->srv);
  ID3D11Texture2D_Release(tex->texture);
  MemoryZeroStruct(tex);
}

