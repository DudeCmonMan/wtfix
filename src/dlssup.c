// Output-size depth and stencil after a DLSS upscale. Stencil goes one bit per pass: D3D11 on NVIDIA can't write a
// stencil value from a shader.
#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dlssup.h"

#define VIEWS D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT

// Output pixel p takes render pixel floor((p + 0.5) * scale + jitter), the one whose jittered sample landed nearest.
// Stencil edges taken that way jitter by a render pixel a frame (outlines flicker), so each bit's coverage is averaged
// over frames along the motion vectors, clamped to the 3x3 render pixels around, with hysteresis; fast motion weighs
// the new frame more so edges don't lag.
static const char up_source[] =
    "cbuffer U : register(b0) { float2 scale; float2 jitter; float2 last; uint bit; uint reset; float2 out_size; float2 pad; };\n"
    "Texture2D<float> scene_depth : register(t0);\n"
    "Texture2D<float> hw_depth : register(t1);\n"
    "Texture2D<uint2> stencil : register(t2);\n"
    "Texture2D<float2> motion : register(t3);\n"
    "Texture2D<float4> prev_low : register(t4);\n"
    "Texture2D<float4> prev_high : register(t5);\n"
    "Texture2D<uint> prev_bits : register(t6);\n"
    "Texture2D<uint> bits : register(t7);\n"
    "SamplerState linear_clamp : register(s0);\n"
    "RWTexture2D<float4> low : register(u0);\n"
    "RWTexture2D<float4> high : register(u1);\n"
    "RWTexture2D<uint> decided : register(u2);\n"
    "float4 vs_main(uint id : SV_VertexID) : SV_Position {\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "int2 source(float2 pos) { return (int2)clamp(floor(pos * scale + jitter), 0, last); }\n"
    "struct Depths { float scene : SV_Target0; float hw : SV_Depth; };\n"
    "Depths depth_main(float4 pos : SV_Position) {\n"
    "  int2 s = source(pos.xy);\n"
    "  Depths o;\n"
    "  o.scene = scene_depth[s];\n"
    "  o.hw = hw_depth[s];\n"
    "  return o;\n"
    "}\n"
    "[numthreads(8, 8, 1)] void cover_main(uint3 id : SV_DispatchThreadID) {\n"
    "  if (id.x >= (uint)out_size.x || id.y >= (uint)out_size.y) return;\n"
    "  float2 pos = id.xy + 0.5;\n"
    "  int2 s = source(pos);\n"
    "  uint now = stencil[s].g, every = 255, some = 0;\n"
    "  float moved = 0;\n"
    "  for (int y = -1; y <= 1; y++)\n"
    "    for (int x = -1; x <= 1; x++) {\n"
    "      int2 n = clamp(s + int2(x, y), 0, (int2)last);\n"
    "      uint v = stencil[n].g;\n"
    "      every &= v;\n"
    "      some |= v;\n"
    "      moved = max(moved, length(motion[n] * out_size));\n"
    "    }\n"
    "  float weight = lerp(0.1, 0.8, saturate(moved));\n"
    "  float2 uv = pos / out_size + motion[s];\n"
    "  bool fresh = reset != 0 || uv.x < 0 || uv.y < 0 || uv.x > 1 || uv.y > 1;\n"
    "  float4 hl = prev_low.SampleLevel(linear_clamp, uv, 0), hh = prev_high.SampleLevel(linear_clamp, uv, 0);\n"
    "  uint was = prev_bits[clamp((int2)(uv * out_size), 0, (int2)out_size - 1)];\n"
    "  float c[8] = {hl.x, hl.y, hl.z, hl.w, hh.x, hh.y, hh.z, hh.w};\n"
    "  uint d = 0;\n"
    "  [unroll] for (uint k = 0; k < 8; k++) {\n"
    "    float lo = (every >> k) & 1, hi = (some >> k) & 1, cur = (now >> k) & 1;\n"
    "    c[k] = fresh ? cur : lerp(clamp(c[k], lo, hi), cur, weight);\n"
    "    bool on = lo == hi ? lo > 0 : fresh ? cur > 0 : ((was >> k) & 1) ? c[k] > 0.35 : c[k] > 0.65;\n"
    "    d |= (uint)on << k;\n"
    "  }\n"
    "  low[id.xy] = float4(c[0], c[1], c[2], c[3]);\n"
    "  high[id.xy] = float4(c[4], c[5], c[6], c[7]);\n"
    "  decided[id.xy] = d;\n"
    "}\n"
    "void stencil_main(float4 pos : SV_Position) {\n"
    "  if (((bits[(uint2)pos.xy] >> bit) & 1) == 0) discard;\n"
    "}\n";

typedef struct {
    float scale[2], jitter[2], last[2];
    unsigned bit, reset;
    float out_size[2], pad[2];
} UpConstants;

static ID3D11Device *device;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps_depth, *ps_stencil;
static ID3D11Buffer *constants;
static ID3D11DepthStencilState *depth_state, *stencil_state[8];
static ID3D11RasterizerState *raster;
static ID3D11ComputeShader *cs_cover;
static ID3D11SamplerState *linear_clamp;
// Stencil coverage history, two frames ping-ponged: bits 0-3 and 4-7 (RGBA8) and the bits decided.
enum { COVER_LOW, COVER_HIGH, COVER_BITS, COVERS };
static struct {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11UnorderedAccessView *uav;
} cover[2][COVERS];
static int cover_w, cover_h, cover_now;

static ID3DBlob *compile(const char *entry, const char *profile, char *error, int size) {
    ID3DBlob *code = NULL, *errors = NULL;
    if (FAILED(D3DCompile(up_source, sizeof up_source - 1, "dlss_up", NULL, NULL, entry, profile,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        snprintf(error, size, "%s: %s", entry, errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
        code = NULL;
    }
    if (errors) ID3D10Blob_Release(errors);
    return code;
}

bool wt_dlss_up_start(ID3D11Device *d, char *error, int size) {
    device = d;
    *error = 0;
    ID3DBlob *v = compile("vs_main", "vs_5_0", error, size), *pd = v ? compile("depth_main", "ps_5_0", error, size) : NULL;
    ID3DBlob *pst = pd ? compile("stencil_main", "ps_5_0", error, size) : NULL;
    ID3DBlob *cs = pst ? compile("cover_main", "cs_5_0", error, size) : NULL;
    bool ok = cs &&
              SUCCEEDED(ID3D11Device_CreateComputeShader(device, ID3D10Blob_GetBufferPointer(cs),
                                                         ID3D10Blob_GetBufferSize(cs), NULL, &cs_cover)) &&
              SUCCEEDED(ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(v), ID3D10Blob_GetBufferSize(v),
                                                        NULL, &vs)) &&
              SUCCEEDED(ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(pd),
                                                       ID3D10Blob_GetBufferSize(pd), NULL, &ps_depth)) &&
              SUCCEEDED(ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(pst),
                                                       ID3D10Blob_GetBufferSize(pst), NULL, &ps_stencil));
    if (v) ID3D10Blob_Release(v);
    if (pd) ID3D10Blob_Release(pd);
    if (pst) ID3D10Blob_Release(pst);
    if (cs) ID3D10Blob_Release(cs);
    if (!ok) {
        if (!*error) snprintf(error, size, "depth shaders");
        return false;
    }
    D3D11_BUFFER_DESC bd = {sizeof(UpConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE,
                            0, 0};
    D3D11_DEPTH_STENCIL_DESC dd = {0};
    dd.DepthEnable = TRUE, dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL, dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    D3D11_RASTERIZER_DESC rd = {0};
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.DepthClipEnable = TRUE;
    D3D11_SAMPLER_DESC sm = {0};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR, sm.MaxLOD = D3D11_FLOAT32_MAX;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ok = SUCCEEDED(ID3D11Device_CreateBuffer(device, &bd, NULL, &constants)) &&
         SUCCEEDED(ID3D11Device_CreateSamplerState(device, &sm, &linear_clamp)) &&
         SUCCEEDED(ID3D11Device_CreateDepthStencilState(device, &dd, &depth_state)) &&
         SUCCEEDED(ID3D11Device_CreateRasterizerState(device, &rd, &raster));
    for (int k = 0; ok && k < 8; k++) {
        D3D11_DEPTH_STENCILOP_DESC replace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE,
                                              D3D11_COMPARISON_ALWAYS};
        D3D11_DEPTH_STENCIL_DESC sd = {0};
        sd.DepthFunc = D3D11_COMPARISON_ALWAYS, sd.StencilEnable = TRUE, sd.StencilReadMask = 0xFF;
        sd.StencilWriteMask = (UINT8)(1 << k), sd.FrontFace = replace, sd.BackFace = replace;
        ok = SUCCEEDED(ID3D11Device_CreateDepthStencilState(device, &sd, &stencil_state[k]));
    }
    if (!ok) snprintf(error, size, "depth pass states");
    return ok;
}

#define BUFFERS D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
#define SAMPLERS D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT
#define VERTEX_BUFFERS 16

struct WtD3dState {
    ID3D11RenderTargetView *rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11DepthStencilView *dsv;
    ID3D11DepthStencilState *depth;
    ID3D11BlendState *blend;
    ID3D11RasterizerState *raster;
    ID3D11InputLayout *layout;
    ID3D11Buffer *vertex[VERTEX_BUFFERS], *index;
    UINT strides[VERTEX_BUFFERS], offsets[VERTEX_BUFFERS], index_offset;
    DXGI_FORMAT index_format;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Buffer *ps_constants[BUFFERS], *vs_constants[BUFFERS];
    ID3D11SamplerState *ps_samplers[SAMPLERS], *vs_samplers[SAMPLERS];
    ID3D11ShaderResourceView *ps_views[VIEWS], *vs_views[VIEWS];
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_PRIMITIVE_TOPOLOGY topology;
    FLOAT factor[4];
    UINT ref, sample_mask, nviewports;
};
typedef WtD3dState Pipeline;

// Heaps rebinds only what it thinks changed: restore everything, even SRVs D3D11 unbinds when bound as output.
static void save(ID3D11DeviceContext *c, Pipeline *p) {
    memset(p, 0, sizeof *p);
    ID3D11DeviceContext_OMGetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, p->rtv, &p->dsv);
    ID3D11DeviceContext_OMGetDepthStencilState(c, &p->depth, &p->ref);
    ID3D11DeviceContext_OMGetBlendState(c, &p->blend, p->factor, &p->sample_mask);
    ID3D11DeviceContext_RSGetState(c, &p->raster);
    ID3D11DeviceContext_RSGetViewports(c, &p->nviewports, NULL);
    if (p->nviewports) ID3D11DeviceContext_RSGetViewports(c, &p->nviewports, p->viewports);
    ID3D11DeviceContext_IAGetInputLayout(c, &p->layout);
    ID3D11DeviceContext_IAGetPrimitiveTopology(c, &p->topology);
    ID3D11DeviceContext_IAGetVertexBuffers(c, 0, VERTEX_BUFFERS, p->vertex, p->strides, p->offsets);
    ID3D11DeviceContext_IAGetIndexBuffer(c, &p->index, &p->index_format, &p->index_offset);
    ID3D11DeviceContext_VSGetShader(c, &p->vs, NULL, NULL);
    ID3D11DeviceContext_PSGetShader(c, &p->ps, NULL, NULL);
    ID3D11DeviceContext_PSGetConstantBuffers(c, 0, BUFFERS, p->ps_constants);
    ID3D11DeviceContext_VSGetConstantBuffers(c, 0, BUFFERS, p->vs_constants);
    ID3D11DeviceContext_PSGetSamplers(c, 0, SAMPLERS, p->ps_samplers);
    ID3D11DeviceContext_VSGetSamplers(c, 0, SAMPLERS, p->vs_samplers);
    ID3D11DeviceContext_PSGetShaderResources(c, 0, VIEWS, p->ps_views);
    ID3D11DeviceContext_VSGetShaderResources(c, 0, VIEWS, p->vs_views);
}

#define RELEASE(type, x) if (x) type##_Release(x)

static void restore(ID3D11DeviceContext *c, Pipeline *p) {
    ID3D11DeviceContext_OMSetRenderTargets(c, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, p->rtv, p->dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(c, p->depth, p->ref);
    ID3D11DeviceContext_OMSetBlendState(c, p->blend, p->factor, p->sample_mask);
    ID3D11DeviceContext_RSSetState(c, p->raster);
    ID3D11DeviceContext_RSSetViewports(c, p->nviewports, p->viewports);
    ID3D11DeviceContext_IASetInputLayout(c, p->layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, p->topology);
    ID3D11DeviceContext_IASetVertexBuffers(c, 0, VERTEX_BUFFERS, p->vertex, p->strides, p->offsets);
    ID3D11DeviceContext_IASetIndexBuffer(c, p->index, p->index_format, p->index_offset);
    ID3D11DeviceContext_VSSetShader(c, p->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, p->ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(c, 0, BUFFERS, p->ps_constants);
    ID3D11DeviceContext_VSSetConstantBuffers(c, 0, BUFFERS, p->vs_constants);
    ID3D11DeviceContext_PSSetSamplers(c, 0, SAMPLERS, p->ps_samplers);
    ID3D11DeviceContext_VSSetSamplers(c, 0, SAMPLERS, p->vs_samplers);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, VIEWS, p->ps_views);
    ID3D11DeviceContext_VSSetShaderResources(c, 0, VIEWS, p->vs_views);
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) RELEASE(ID3D11RenderTargetView, p->rtv[i]);
    for (int i = 0; i < VIEWS; i++) {
        RELEASE(ID3D11ShaderResourceView, p->ps_views[i]);
        RELEASE(ID3D11ShaderResourceView, p->vs_views[i]);
    }
    for (int i = 0; i < BUFFERS; i++) {
        RELEASE(ID3D11Buffer, p->ps_constants[i]);
        RELEASE(ID3D11Buffer, p->vs_constants[i]);
    }
    for (int i = 0; i < SAMPLERS; i++) {
        RELEASE(ID3D11SamplerState, p->ps_samplers[i]);
        RELEASE(ID3D11SamplerState, p->vs_samplers[i]);
    }
    for (int i = 0; i < VERTEX_BUFFERS; i++) RELEASE(ID3D11Buffer, p->vertex[i]);
    RELEASE(ID3D11Buffer, p->index);
    RELEASE(ID3D11DepthStencilView, p->dsv);
    RELEASE(ID3D11DepthStencilState, p->depth);
    RELEASE(ID3D11BlendState, p->blend);
    RELEASE(ID3D11RasterizerState, p->raster);
    RELEASE(ID3D11InputLayout, p->layout);
    RELEASE(ID3D11VertexShader, p->vs);
    RELEASE(ID3D11PixelShader, p->ps);
}

WtD3dState *wt_d3d_save(ID3D11DeviceContext *c) {
    WtD3dState *s = malloc(sizeof *s);
    if (s) save(c, s);
    return s;
}

void wt_d3d_restore(ID3D11DeviceContext *c, WtD3dState *s) {
    if (!s) return;
    restore(c, s);
    free(s);
}

static bool expect(ID3D11Resource *res, const char *name, int w, int h, DXGI_FORMAT f1, DXGI_FORMAT f2, UINT bind,
                   char *error, int size) {
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(res, &dim);
    D3D11_TEXTURE2D_DESC d;
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)res, &d);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D && (int)d.Width == w && (int)d.Height == h &&
        (d.Format == f1 || d.Format == f2) && (d.BindFlags & bind) == bind && d.SampleDesc.Count == 1)
        return true;
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) snprintf(error, size, "%s is not a 2D texture", name);
    else
        snprintf(error, size, "%s is %ux%u format %d bind %#x (want %dx%d format %d, bind %#x)", name, d.Width, d.Height,
                 (int)d.Format, d.BindFlags, w, h, (int)f1, bind);
    return false;
}

static void release_cover(void) {
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < COVERS; i++) {
            RELEASE(ID3D11UnorderedAccessView, cover[f][i].uav);
            RELEASE(ID3D11ShaderResourceView, cover[f][i].srv);
            RELEASE(ID3D11Texture2D, cover[f][i].tex);
            cover[f][i].uav = NULL, cover[f][i].srv = NULL, cover[f][i].tex = NULL;
        }
    cover_w = cover_h = 0;
}

static bool make_cover(int w, int h) {
    release_cover();
    for (int f = 0; f < 2; f++)
        for (int i = 0; i < COVERS; i++) {
            D3D11_TEXTURE2D_DESC d = {0};
            d.Width = w, d.Height = h, d.MipLevels = 1, d.ArraySize = 1, d.SampleDesc.Count = 1;
            d.Format = i == COVER_BITS ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            if (FAILED(ID3D11Device_CreateTexture2D(device, &d, NULL, &cover[f][i].tex)) ||
                FAILED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)cover[f][i].tex, NULL,
                                                             &cover[f][i].srv)) ||
                FAILED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource *)cover[f][i].tex, NULL,
                                                              &cover[f][i].uav))) {
                release_cover();
                return false;
            }
        }
    cover_w = w, cover_h = h;
    return true;
}

bool wt_dlss_up_prepare(int out_w, int out_h) {
    return (out_w == cover_w && out_h == cover_h) || make_cover(out_w, out_h);
}

static void set_constants(ID3D11DeviceContext *c, const UpConstants *k) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(c, (ID3D11Resource *)constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, k, sizeof *k);
        ID3D11DeviceContext_Unmap(c, (ID3D11Resource *)constants, 0);
    }
}

bool wt_dlss_up_run(ID3D11DeviceContext *c, const WtDlssUp *up, char *error, int size) {
    const UINT srv = D3D11_BIND_SHADER_RESOURCE, ds = D3D11_BIND_DEPTH_STENCIL;
    if (!expect(up->depth, "scene depth", up->w, up->h, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, srv, error, size) ||
        !expect(up->depth_stencil, "scene depth-stencil", up->w, up->h, DXGI_FORMAT_R24G8_TYPELESS,
                DXGI_FORMAT_R24G8_TYPELESS, srv | ds, error, size) ||
        !expect(up->out_depth, "output depth", up->out_w, up->out_h, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT,
                D3D11_BIND_RENDER_TARGET, error, size) ||
        !expect(up->out_depth_stencil, "output depth-stencil", up->out_w, up->out_h, DXGI_FORMAT_R24G8_TYPELESS,
                DXGI_FORMAT_D24_UNORM_S8_UINT, ds, error, size))
        return false;
    bool fresh = up->reset || up->out_w != cover_w || up->out_h != cover_h;
    if ((up->out_w != cover_w || up->out_h != cover_h) && !make_cover(up->out_w, up->out_h)) {
        snprintf(error, size, "could not make the stencil history (%dx%d)", up->out_w, up->out_h);
        return false;
    }
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11ShaderResourceView *views[3] = {NULL, NULL, NULL}, *motion = NULL;
    D3D11_DEPTH_STENCIL_VIEW_DESC dv = {0};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT, dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC hw = {0}, st;
    hw.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, hw.Texture2D.MipLevels = 1;
    st = hw;
    hw.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS, st.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    bool ok = SUCCEEDED(ID3D11Device_CreateRenderTargetView(device, up->out_depth, NULL, &rtv)) &&
              SUCCEEDED(ID3D11Device_CreateDepthStencilView(device, up->out_depth_stencil, &dv, &dsv)) &&
              SUCCEEDED(ID3D11Device_CreateShaderResourceView(device, up->depth, NULL, &views[0])) &&
              SUCCEEDED(ID3D11Device_CreateShaderResourceView(device, up->depth_stencil, &hw, &views[1])) &&
              SUCCEEDED(ID3D11Device_CreateShaderResourceView(device, up->depth_stencil, &st, &views[2])) &&
              (!up->motion || SUCCEEDED(ID3D11Device_CreateShaderResourceView(device, up->motion, NULL, &motion)));
    if (ok) {
        Pipeline saved;
        save(c, &saved);
        UpConstants k = {{(float)up->w / up->out_w, (float)up->h / up->out_h}, {up->jitter_x, up->jitter_y},
                         {(float)(up->w - 1), (float)(up->h - 1)}, 0, fresh, {(float)up->out_w, (float)up->out_h}, {0, 0}};
        D3D11_VIEWPORT vp = {0, 0, (float)up->out_w, (float)up->out_h, 0, 1};
        ID3D11DeviceContext_IASetInputLayout(c, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(c, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(c, ps_depth, NULL, 0);
        ID3D11DeviceContext_RSSetState(c, raster);
        ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
        ID3D11DeviceContext_OMSetBlendState(c, NULL, NULL, 0xffffffff);
        ID3D11DeviceContext_OMSetRenderTargets(c, 1, &rtv, dsv);
        ID3D11DeviceContext_PSSetShaderResources(c, 0, 3, views);
        ID3D11DeviceContext_PSSetConstantBuffers(c, 0, 1, &constants);
        set_constants(c, &k);
        ID3D11DeviceContext_OMSetDepthStencilState(c, depth_state, 0);
        ID3D11DeviceContext_Draw(c, 3, 0);
        // Heaps never uses the compute stage: its bindings are ours, and cleared after.
        int now = cover_now ^ 1, was = cover_now;
        ID3D11ShaderResourceView *cs_views[7] = {NULL, NULL, views[2], motion, cover[was][COVER_LOW].srv,
                                                 cover[was][COVER_HIGH].srv, cover[was][COVER_BITS].srv};
        ID3D11UnorderedAccessView *uavs[3] = {cover[now][COVER_LOW].uav, cover[now][COVER_HIGH].uav,
                                              cover[now][COVER_BITS].uav};
        ID3D11DeviceContext_CSSetShader(c, cs_cover, NULL, 0);
        ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &constants);
        ID3D11DeviceContext_CSSetSamplers(c, 0, 1, &linear_clamp);
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 7, cs_views);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 3, uavs, NULL);
        ID3D11DeviceContext_Dispatch(c, (up->out_w + 7) / 8, (up->out_h + 7) / 8, 1);
        ID3D11ShaderResourceView *no_views[7] = {NULL, NULL, NULL, NULL, NULL, NULL, NULL};
        ID3D11UnorderedAccessView *no_uavs[3] = {NULL, NULL, NULL};
        ID3D11Buffer *no_buffer = NULL;
        ID3D11SamplerState *no_sampler = NULL;
        ID3D11DeviceContext_CSSetShaderResources(c, 0, 7, no_views);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(c, 0, 3, no_uavs, NULL);
        ID3D11DeviceContext_CSSetConstantBuffers(c, 0, 1, &no_buffer);
        ID3D11DeviceContext_CSSetSamplers(c, 0, 1, &no_sampler);
        ID3D11DeviceContext_CSSetShader(c, NULL, NULL, 0);
        cover_now = now;

        ID3D11DeviceContext_ClearDepthStencilView(c, dsv, D3D11_CLEAR_STENCIL, 1.0f, 0);
        ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, dsv);
        ID3D11DeviceContext_PSSetShader(c, ps_stencil, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(c, 7, 1, &cover[now][COVER_BITS].srv);
        for (k.bit = 0; k.bit < 8; k.bit++) {
            set_constants(c, &k);
            ID3D11DeviceContext_OMSetDepthStencilState(c, stencil_state[k.bit], 0xFF);
            ID3D11DeviceContext_Draw(c, 3, 0);
        }
        restore(c, &saved);
    } else {
        snprintf(error, size, "could not make views of the depth textures");
    }
    for (int i = 0; i < 3; i++) RELEASE(ID3D11ShaderResourceView, views[i]);
    RELEASE(ID3D11ShaderResourceView, motion);
    RELEASE(ID3D11RenderTargetView, rtv);
    RELEASE(ID3D11DepthStencilView, dsv);
    return ok;
}
