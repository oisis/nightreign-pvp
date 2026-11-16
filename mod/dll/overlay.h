// Online counts overlay (show-counts, default on): a few lines of text in the top-right corner, drawn on the
// game's own D3D12 swap chain right before its Present.
//
// How we get there (the game's imports: dxgi CreateDXGIFactory / CreateDXGIFactory1, d3d12 by ordinal):
//  - DllMain patches those two import slots; the factory they return gets CreateSwapChain and
//    CreateSwapChainForHwnd wrapped (vtable, so every factory of that class), which records the game's command
//    queue (the "device" argument of a D3D12 swap chain) and the swap chain, and wraps its Present / Present1.
//  - Present: if there is something to show, our own command allocator + list record one textured quad on the
//    current back buffer (alpha blended) and run on the game's queue before its Present. The text is drawn with
//    GDI (a system font, nothing shipped) into a small bitmap and uploaded only when it changes.
//  - Nothing of the game is replaced: every wrapper calls what was in the slot before (the Steam overlay chains the
//    same way or hooks inside the function). No back-buffer references are kept between frames, so the game's
//    ResizeBuffers is not disturbed; a changed format or buffer count rebuilds our pipeline.
//  - Any failure: logged once, the overlay stays off for the session. Never blocks: if our previous use of a back
//    buffer's allocator has not finished on the GPU, that frame is skipped.
#define COBJMACROS
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>

typedef HRESULT(WINAPI *create_factory_t)(REFIID, void **);
typedef HRESULT(STDMETHODCALLTYPE *create_sc_t)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);
typedef HRESULT(STDMETHODCALLTYPE *create_sc_hwnd_t)(IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
                                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
typedef HRESULT(STDMETHODCALLTYPE *present_t)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *present1_t)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT(WINAPI *serialize_rs_t)(const D3D12_ROOT_SIGNATURE_DESC *, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob **, ID3DBlob **);
typedef HRESULT(WINAPI *compile_t)(const void *, SIZE_T, const char *, const void *, void *, const char *, const char *, UINT, UINT,
                                   ID3DBlob **, ID3DBlob **);

static create_factory_t g_ov_cf, g_ov_cf1;
static create_sc_t g_ov_create_sc;
static create_sc_hwnd_t g_ov_create_sc_hwnd;
static present_t g_ov_present;
static present1_t g_ov_present1;
static volatile int g_ov_off;            // 1: failed or disabled, nothing more is tried
static ID3D12CommandQueue *g_ov_queue;   // the game's (no reference taken: it lives as long as its swap chain)
static IDXGISwapChain *g_ov_sc;
#define OV_W 512
#define OV_H 96
#define OV_MAX_BUF 8
static struct {
    ID3D12Device *dev;
    ID3D12RootSignature *rs;
    ID3D12PipelineState *pso;
    DXGI_FORMAT format;
    UINT buffers;
    ID3D12DescriptorHeap *rtv, *srv;
    UINT rtv_step;
    ID3D12Resource *tex, *upload;
    ID3D12CommandAllocator *alloc[OV_MAX_BUF];
    UINT64 used[OV_MAX_BUF];
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *fence;
    UINT64 fence_value;
    volatile LONG dirty;      // the bitmap changed: upload before the next draw
    volatile int shown;       // something to draw
    int has_text;             // a text was made once
    unsigned char pixels[OV_W * OV_H * 4];  // BGRA, straight alpha
    CRITICAL_SECTION lock;
    int lock_ready;
} g_ov;

static void ov_fail(const char *what, HRESULT hr) {
    if (!g_ov_off) logf("counts overlay: %s failed (0x%08lx): overlay off for this session", what, (unsigned long)hr);
    g_ov_off = 1;
}
static void ov_patch(void **slot, void *hook, void **orig) {
    DWORD old;
    if (*slot == hook || !VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return;
    *orig = *slot;
    *slot = hook;
    VirtualProtect(slot, sizeof *slot, old, &old);
}

// The text: lines from the launcher's FMG (131172..131174, "%d"), drawn with GDI into our bitmap.
// GDI from gdi32.dll at run time (the DLL is built without import libraries beyond the defaults)
static struct {
    HDC(WINAPI *CreateCompatibleDC)(HDC);
    HBITMAP(WINAPI *CreateDIBSection)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
    HGDIOBJ(WINAPI *SelectObject)(HDC, HGDIOBJ);
    HFONT(WINAPI *CreateFontW)(int, int, int, int, int, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, DWORD, LPCWSTR);
    int(WINAPI *SetBkMode)(HDC, int);
    COLORREF(WINAPI *SetTextColor)(HDC, COLORREF);
    BOOL(WINAPI *GdiFlush)(void);
    BOOL(WINAPI *DeleteObject)(HGDIOBJ);
    BOOL(WINAPI *DeleteDC)(HDC);
} gdi;
static int gdi_ready(void) {
    static int state;  // 0 not tried, 1 ready, -1 missing
    if (state) return state > 0;
    HMODULE m = LoadLibraryA("gdi32.dll");
    static const char *const names[9] = {"CreateCompatibleDC", "CreateDIBSection", "SelectObject", "CreateFontW", "SetBkMode",
                                         "SetTextColor", "GdiFlush", "DeleteObject", "DeleteDC"};
    void **f = (void **)&gdi;
    state = m ? 1 : -1;
    for (int i = 0; state > 0 && i < 9; i++)
        if (!(f[i] = (void *)GetProcAddress(m, names[i]))) state = -1;
    return state > 0;
}
static void ov_set_lines(const unsigned short *const *lines, int n) {
    if (g_ov_off || !g_ov.lock_ready || !gdi_ready()) return;
    HDC dc = gdi.CreateCompatibleDC(NULL);
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader, bi.bmiHeader.biWidth = OV_W, bi.bmiHeader.biHeight = -OV_H;
    bi.bmiHeader.biPlanes = 1, bi.bmiHeader.biBitCount = 32, bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = dc ? gdi.CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!bmp || !bits) {
        if (bmp) gdi.DeleteObject(bmp);
        if (dc) gdi.DeleteDC(dc);
        return;
    }
    HGDIOBJ oldb = gdi.SelectObject(dc, bmp);
    HFONT font = gdi.CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HGDIOBJ oldf = font ? gdi.SelectObject(dc, font) : NULL;
    memset(bits, 0, OV_W * OV_H * 4);
    gdi.SetBkMode(dc, TRANSPARENT);
    gdi.SetTextColor(dc, RGB(255, 255, 255));
    for (int i = 0; i < n && i < 3; i++) {
        RECT r = {0, 4 + i * 30, OV_W - 8, 4 + i * 30 + 28};
        DrawTextW(dc, (const wchar_t *)lines[i], -1, &r, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);
    }
    gdi.GdiFlush();
    EnterCriticalSection(&g_ov.lock);
    unsigned char *src = bits, *dst = g_ov.pixels;
    for (int i = 0; i < OV_W * OV_H; i++) {  // white text on black: brightness as alpha, with a soft dark backing
        unsigned a = src[i * 4 + 1];
        dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = 255;
        dst[i * 4 + 3] = (unsigned char)a;
    }
    g_ov.shown = g_ov.has_text = n > 0;
    g_ov.dirty = 1;
    LeaveCriticalSection(&g_ov.lock);
    if (oldf) gdi.SelectObject(dc, oldf);
    if (font) gdi.DeleteObject(font);
    gdi.SelectObject(dc, oldb);
    gdi.DeleteObject(bmp);
    gdi.DeleteDC(dc);
}
static void ov_hide(void) { g_ov.shown = 0; }

static const char OV_HLSL[] =
    "cbuffer c : register(b0) { float4 r; };\n"
    "struct V { float4 p : SV_Position; float2 t : TEXCOORD; };\n"
    "V vs(uint i : SV_VertexID) { V o; float2 u = float2(i & 1, i >> 1); o.t = u;\n"
    "  o.p = float4(lerp(r.x, r.z, u.x), lerp(r.y, r.w, u.y), 0, 1); return o; }\n"
    "Texture2D tx : register(t0); SamplerState sm : register(s0);\n"
    "float4 ps(V v) : SV_Target { float4 c = tx.Sample(sm, v.t); float s = tx.Sample(sm, v.t + float2(-0.003, -0.02)).a;\n"
    "  return float4(c.rgb * c.a, max(c.a, s * 0.6)); }\n";

static int ov_build(DXGI_FORMAT format, UINT buffers) {
    HRESULT hr;
    if (!g_ov.dev && FAILED(hr = ID3D12CommandQueue_GetDevice(g_ov_queue, &IID_ID3D12Device, (void **)&g_ov.dev))) return ov_fail("GetDevice", hr), 0;
    ID3D12Device *d = g_ov.dev;
    if (!g_ov.rs) {
        HMODULE m = GetModuleHandleA("d3d12.dll");
        serialize_rs_t ser = m ? (serialize_rs_t)(void *)GetProcAddress(m, "D3D12SerializeRootSignature") : NULL;
        if (!ser) return ov_fail("D3D12SerializeRootSignature", 0), 0;
        D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER params[2] = {0};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1, params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC smp = {0};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX, smp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {2, params, 1, &smp, 0};
        ID3DBlob *blob = NULL, *err = NULL;
        if (FAILED(hr = ser(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return ov_fail("root signature", hr), 0;
        hr = ID3D12Device_CreateRootSignature(d, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature,
                                              (void **)&g_ov.rs);
        ID3D10Blob_Release(blob);
        if (err) ID3D10Blob_Release(err);
        if (FAILED(hr)) return ov_fail("CreateRootSignature", hr), 0;
    }
    if (!g_ov.pso || g_ov.format != format) {
        HMODULE c = LoadLibraryA("d3dcompiler_47.dll");
        compile_t compile = c ? (compile_t)(void *)GetProcAddress(c, "D3DCompile") : NULL;
        if (!compile) return ov_fail("d3dcompiler_47 D3DCompile", 0), 0;
        ID3DBlob *vs = NULL, *ps = NULL, *err = NULL;
        if (FAILED(hr = compile(OV_HLSL, sizeof OV_HLSL - 1, "ov", NULL, NULL, "vs", "vs_5_0", 0, 0, &vs, &err))) return ov_fail("vertex shader", hr), 0;
        if (err) ID3D10Blob_Release(err), err = NULL;
        if (FAILED(hr = compile(OV_HLSL, sizeof OV_HLSL - 1, "ov", NULL, NULL, "ps", "ps_5_0", 0, 0, &ps, &err))) {
            ID3D10Blob_Release(vs);
            return ov_fail("pixel shader", hr), 0;
        }
        if (err) ID3D10Blob_Release(err);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p = {0};
        p.pRootSignature = g_ov.rs;
        p.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vs), p.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vs);
        p.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(ps), p.PS.BytecodeLength = ID3D10Blob_GetBufferSize(ps);
        D3D12_RENDER_TARGET_BLEND_DESC *b = &p.BlendState.RenderTarget[0];
        b->BlendEnable = TRUE;
        b->SrcBlend = D3D12_BLEND_ONE, b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA, b->BlendOp = D3D12_BLEND_OP_ADD;
        b->SrcBlendAlpha = D3D12_BLEND_ONE, b->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA, b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        b->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        p.SampleMask = 0xffffffff;
        p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID, p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        p.RasterizerState.DepthClipEnable = TRUE;
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.NumRenderTargets = 1, p.RTVFormats[0] = format, p.SampleDesc.Count = 1;
        if (g_ov.pso) ID3D12PipelineState_Release(g_ov.pso), g_ov.pso = NULL;
        hr = ID3D12Device_CreateGraphicsPipelineState(d, &p, &IID_ID3D12PipelineState, (void **)&g_ov.pso);
        ID3D10Blob_Release(vs), ID3D10Blob_Release(ps);
        if (FAILED(hr)) return ov_fail("CreateGraphicsPipelineState", hr), 0;
        g_ov.format = format;
    }
    if (!g_ov.rtv || g_ov.buffers != buffers) {
        if (g_ov.rtv) ID3D12DescriptorHeap_Release(g_ov.rtv), g_ov.rtv = NULL;
        D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, OV_MAX_BUF, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(hr = ID3D12Device_CreateDescriptorHeap(d, &hd, &IID_ID3D12DescriptorHeap, (void **)&g_ov.rtv))) return ov_fail("RTV heap", hr), 0;
        g_ov.rtv_step = ID3D12Device_GetDescriptorHandleIncrementSize(d, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        g_ov.buffers = buffers;
    }
    if (!g_ov.tex) {
        D3D12_HEAP_PROPERTIES def = {D3D12_HEAP_TYPE_DEFAULT, 0, 0, 0, 0}, up = {D3D12_HEAP_TYPE_UPLOAD, 0, 0, 0, 0};
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, td.Width = OV_W, td.Height = OV_H, td.DepthOrArraySize = 1, td.MipLevels = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM, td.SampleDesc.Count = 1;
        if (FAILED(hr = ID3D12Device_CreateCommittedResource(d, &def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                             &IID_ID3D12Resource, (void **)&g_ov.tex)))
            return ov_fail("texture", hr), 0;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, bd.Width = OV_W * 4 * OV_H, bd.Height = 1, bd.DepthOrArraySize = 1, bd.MipLevels = 1;
        bd.SampleDesc.Count = 1, bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(hr = ID3D12Device_CreateCommittedResource(d, &up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                             &IID_ID3D12Resource, (void **)&g_ov.upload)))
            return ov_fail("upload buffer", hr), 0;
        D3D12_DESCRIPTOR_HEAP_DESC sd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (FAILED(hr = ID3D12Device_CreateDescriptorHeap(d, &sd, &IID_ID3D12DescriptorHeap, (void **)&g_ov.srv))) return ov_fail("SRV heap", hr), 0;
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        g_ov.srv->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_ov.srv, &h);
        ID3D12Device_CreateShaderResourceView(d, g_ov.tex, NULL, h);
        if (FAILED(hr = ID3D12Device_CreateFence(d, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&g_ov.fence))) return ov_fail("fence", hr), 0;
        for (int i = 0; i < OV_MAX_BUF; i++)
            if (FAILED(hr = ID3D12Device_CreateCommandAllocator(d, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&g_ov.alloc[i])))
                return ov_fail("command allocator", hr), 0;
        if (FAILED(hr = ID3D12Device_CreateCommandList(d, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ov.alloc[0], NULL, &IID_ID3D12GraphicsCommandList,
                                                       (void **)&g_ov.list)))
            return ov_fail("command list", hr), 0;
        ID3D12GraphicsCommandList_Close(g_ov.list);
    }
    return 1;
}
static void ov_barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, b.Transition.StateBefore = from, b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(g_ov.list, 1, &b);
}
static void ov_draw(IDXGISwapChain *sc) {
    if (g_ov_off || !g_ov.shown || sc != g_ov_sc || !g_ov_queue || !setting(S_SHOW_COUNTS)) return;
    static int tex_ready;  // the texture went to PIXEL_SHADER_RESOURCE once
    IDXGISwapChain3 *sc3 = NULL;
    if (FAILED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain3, (void **)&sc3))) return ov_fail("IDXGISwapChain3", 0);
    DXGI_SWAP_CHAIN_DESC desc;
    UINT idx = IDXGISwapChain3_GetCurrentBackBufferIndex(sc3);
    IDXGISwapChain3_Release(sc3);
    if (FAILED(IDXGISwapChain_GetDesc(sc, &desc)) || desc.BufferCount > OV_MAX_BUF || idx >= desc.BufferCount) return;
    if (!ov_build(desc.BufferDesc.Format, desc.BufferCount)) return;
    if (ID3D12Fence_GetCompletedValue(g_ov.fence) < g_ov.used[idx]) return;  // our last use of this allocator still running
    ID3D12Resource *back = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(sc, idx, &IID_ID3D12Resource, (void **)&back))) return;
    ID3D12CommandAllocator_Reset(g_ov.alloc[idx]);
    ID3D12GraphicsCommandList_Reset(g_ov.list, g_ov.alloc[idx], g_ov.pso);
    // new text: into the upload buffer (only once the GPU is done with our earlier work), then the texture
    if (g_ov.dirty && ID3D12Fence_GetCompletedValue(g_ov.fence) >= g_ov.fence_value && InterlockedExchange(&g_ov.dirty, 0)) {
        void *map = NULL;
        D3D12_RANGE none = {0, 0};
        if (SUCCEEDED(ID3D12Resource_Map(g_ov.upload, 0, &none, &map))) {
            EnterCriticalSection(&g_ov.lock);
            memcpy(map, g_ov.pixels, sizeof g_ov.pixels);
            LeaveCriticalSection(&g_ov.lock);
            ID3D12Resource_Unmap(g_ov.upload, 0, NULL);
        }
        if (tex_ready) ov_barrier(g_ov.tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = g_ov.tex, dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = g_ov.upload, src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM, src.PlacedFootprint.Footprint.Width = OV_W;
        src.PlacedFootprint.Footprint.Height = OV_H, src.PlacedFootprint.Footprint.Depth = 1, src.PlacedFootprint.Footprint.RowPitch = OV_W * 4;
        ID3D12GraphicsCommandList_CopyTextureRegion(g_ov.list, &dst, 0, 0, 0, &src, NULL);
        ov_barrier(g_ov.tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        tex_ready = 1;
    }
    if (!tex_ready) {  // nothing uploaded yet
        ID3D12GraphicsCommandList_Close(g_ov.list);
        ID3D12Resource_Release(back);
        return;
    }
    ov_barrier(back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    g_ov.rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(g_ov.rtv, &rtv);
    rtv.ptr += (SIZE_T)idx * g_ov.rtv_step;
    ID3D12Device_CreateRenderTargetView(g_ov.dev, back, NULL, rtv);
    float w = (float)desc.BufferDesc.Width, h = (float)desc.BufferDesc.Height;
    D3D12_VIEWPORT vp = {0, 0, w, h, 0, 1};
    D3D12_RECT sr = {0, 0, (LONG)w, (LONG)h};
    // top right, scaled with the output height (1080 lines: 1:1 pixels), a margin of 2 % of the height
    float s = h / 1080.0f, pw = OV_W * s, ph = OV_H * s, m = h * 0.02f;
    float r[4] = {(w - m - pw) / w * 2 - 1, 1 - m / h * 2, (w - m) / w * 2 - 1, 1 - (m + ph) / h * 2};
    ID3D12GraphicsCommandList_OMSetRenderTargets(g_ov.list, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_RSSetViewports(g_ov.list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(g_ov.list, 1, &sr);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(g_ov.list, g_ov.rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(g_ov.list, 1, &g_ov.srv);
    D3D12_GPU_DESCRIPTOR_HANDLE gh;
    g_ov.srv->lpVtbl->GetGPUDescriptorHandleForHeapStart(g_ov.srv, &gh);
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(g_ov.list, 0, 4, r, 0);
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(g_ov.list, 1, gh);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(g_ov.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_DrawInstanced(g_ov.list, 4, 1, 0, 0);
    ov_barrier(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    if (FAILED(ID3D12GraphicsCommandList_Close(g_ov.list))) {
        ID3D12Resource_Release(back);
        return ov_fail("command list close", 0);
    }
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)g_ov.list};
    ID3D12CommandQueue_ExecuteCommandLists(g_ov_queue, 1, lists);
    ID3D12CommandQueue_Signal(g_ov_queue, g_ov.fence, ++g_ov.fence_value);
    g_ov.used[idx] = g_ov.fence_value;
    ID3D12Resource_Release(back);
    static int told;
    if (!told++) logf("counts overlay: first frame drawn (%ux%u, format %u, %u buffers)", (unsigned)w, (unsigned)h, (unsigned)desc.BufferDesc.Format,
                      (unsigned)desc.BufferCount);
}
static HRESULT STDMETHODCALLTYPE ov_present_hook(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) ov_draw(sc);
    return g_ov_present(sc, sync, flags);
}
static HRESULT STDMETHODCALLTYPE ov_present1_hook(IDXGISwapChain1 *sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS *p) {
    if (!(flags & DXGI_PRESENT_TEST)) ov_draw((IDXGISwapChain *)sc);
    return g_ov_present1(sc, sync, flags, p);
}
static void ov_swap_chain(IUnknown *device, IDXGISwapChain *sc) {
    ID3D12CommandQueue *q = NULL;
    if (!device || !sc || FAILED(device->lpVtbl->QueryInterface(device, &IID_ID3D12CommandQueue, (void **)&q))) return;  // not a D3D12 swap chain
    ID3D12CommandQueue_Release(q);  // the game keeps it; we use it while its swap chain lives
    g_ov_queue = q, g_ov_sc = sc;
    IDXGISwapChainVtbl *vt = (IDXGISwapChainVtbl *)sc->lpVtbl;
    ov_patch((void **)&vt->Present, (void *)ov_present_hook, (void **)&g_ov_present);
    IDXGISwapChain1 *sc1 = NULL;
    if (SUCCEEDED(IDXGISwapChain_QueryInterface(sc, &IID_IDXGISwapChain1, (void **)&sc1))) {
        IDXGISwapChain1Vtbl *v1 = (IDXGISwapChain1Vtbl *)sc1->lpVtbl;
        ov_patch((void **)&v1->Present1, (void *)ov_present1_hook, (void **)&g_ov_present1);
        IDXGISwapChain1_Release(sc1);
    }
    logf("counts overlay: the game's swap chain %p on queue %p: Present wrapped", (void *)sc, (void *)q);
}
static HRESULT STDMETHODCALLTYPE ov_create_sc_hook(IDXGIFactory *f, IUnknown *dev, DXGI_SWAP_CHAIN_DESC *d, IDXGISwapChain **out) {
    HRESULT hr = g_ov_create_sc(f, dev, d, out);
    if (SUCCEEDED(hr) && out && *out && !g_ov_off) ov_swap_chain(dev, *out);
    return hr;
}
static HRESULT STDMETHODCALLTYPE ov_create_sc_hwnd_hook(IDXGIFactory2 *f, IUnknown *dev, HWND w, const DXGI_SWAP_CHAIN_DESC1 *d,
                                                        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fd, IDXGIOutput *o, IDXGISwapChain1 **out) {
    HRESULT hr = g_ov_create_sc_hwnd(f, dev, w, d, fd, o, out);
    if (SUCCEEDED(hr) && out && *out && !g_ov_off) ov_swap_chain(dev, (IDXGISwapChain *)*out);
    return hr;
}
static void ov_factory(void *factory) {
    IDXGIFactory *f = factory;
    if (!f) return;
    IDXGIFactoryVtbl *vt = f->lpVtbl;
    ov_patch((void **)&vt->CreateSwapChain, (void *)ov_create_sc_hook, (void **)&g_ov_create_sc);
    IDXGIFactory2 *f2 = NULL;
    if (SUCCEEDED(IDXGIFactory_QueryInterface(f, &IID_IDXGIFactory2, (void **)&f2))) {
        IDXGIFactory2Vtbl *v2 = f2->lpVtbl;
        ov_patch((void **)&v2->CreateSwapChainForHwnd, (void *)ov_create_sc_hwnd_hook, (void **)&g_ov_create_sc_hwnd);
        IDXGIFactory2_Release(f2);
    }
}
static HRESULT WINAPI ov_cf_hook(REFIID id, void **out) {
    HRESULT hr = g_ov_cf(id, out);
    if (SUCCEEDED(hr) && out && !g_ov_off) ov_factory(*out);
    return hr;
}
static HRESULT WINAPI ov_cf1_hook(REFIID id, void **out) {
    HRESULT hr = g_ov_cf1(id, out);
    if (SUCCEEDED(hr) && out && !g_ov_off) ov_factory(*out);
    return hr;
}
static void ov_install(void) {  // DllMain: the exe's import slots of dxgi's two factory functions
    InitializeCriticalSection(&g_ov.lock), g_ov.lock_ready = 1;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(g_base + dir.VirtualAddress); dir.VirtualAddress && d->Name; d++) {
        if (_stricmp(g_base + d->Name, "dxgi.dll") || !d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(g_base + d->OriginalFirstThunk);
        for (size_t i = 0; names[i].u1.Ordinal; i++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)) continue;
            const char *n = ((IMAGE_IMPORT_BY_NAME *)(g_base + names[i].u1.Ordinal))->Name;
            void **slot = (void **)(g_base + d->FirstThunk) + i;
            if (!strcmp(n, "CreateDXGIFactory")) ov_patch(slot, (void *)ov_cf_hook, (void **)&g_ov_cf);
            else if (!strcmp(n, "CreateDXGIFactory1")) ov_patch(slot, (void *)ov_cf1_hook, (void **)&g_ov_cf1);
        }
    }
}
