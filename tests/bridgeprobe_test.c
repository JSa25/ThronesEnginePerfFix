// Test for bridgeprobe.c: the test exe stands in for empire.retail.dll (its own GetProcAddress import is hooked, its
// code is the "game" caller). It renders frames like the game (offscreen scene with an R24G8 depth, a camera constant
// buffer written with Map, the scene copied into the back buffer, UI drawn on top, Present), creates bridge_go.txt and
// checks the capture: target changes with the back buffer and the depth, back-buffer dumps, the camera writes with the
// test's view-projection, and the game callers. A probe that misses the camera or the targets fails it.
// usage: bridgeprobe_test.exe
#include "../bridgeprobe.c"
#include <stdarg.h>
#include <stdlib.h>

static int saw_left;
void (*g_dev_di8_created)(void *di);   // proxy.c's, in the DLL
static void tlog(const char *fmt, ...) {
    if (strstr(fmt, "left to it")) saw_left = 1;
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    putchar('\n');
}

static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID iid_tex2d = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
// another mod's hooks put on top of the probe's (like the Steam overlay's): each calls what the slot held as its original
static d_fn f_d_orig;
static unsigned f_d_calls;
static void STDMETHODCALLTYPE f_d(ID3D11DeviceContext *c, UINT a, UINT b) { f_d_calls++; f_d_orig(c, a, b); }
static HRESULT(STDMETHODCALLTYPE *f_present_orig)(IDXGISwapChain *, UINT, UINT);
static HRESULT STDMETHODCALLTYPE f_present(IDXGISwapChain *sc, UINT s, UINT fl) { return f_present_orig(sc, s, fl); }
// what the probe took for DXGI's Present leads back into the hooks once (Attila: overlay -> UI mod -> probe)
static HRESULT(STDMETHODCALLTYPE *real_present)(IDXGISwapChain *, UINT, UINT);
static HRESULT STDMETHODCALLTYPE loop_present(IDXGISwapChain *sc, UINT s, UINT fl) { IDXGISwapChain_Present(sc, s, fl); return real_present(sc, s, fl); }

#define CHECK(c, ...) do { if (!(c)) { printf("FAILED: " __VA_ARGS__); putchar('\n'); return 1; } } while (0)

int main(void) {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    swprintf(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"bridgeprobe_test_%lu", GetCurrentProcessId());
    CreateDirectoryW(dir, NULL);
#ifndef NOPROBE
    CHECK(install_bridgeprobe(GetModuleHandleW(NULL), dir, tlog), "install");
#endif

    // the game's way: GetProcAddress (now hooked in this exe's import table)
    HMODULE d3d = LoadLibraryW(L"d3d11.dll"), dxgi = LoadLibraryW(L"dxgi.dll");
    create_fn create = (create_fn)(void *)GetProcAddress(d3d, "D3D11CreateDevice");
    factory_fn factory = (factory_fn)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
#ifndef NOPROBE
    CHECK(create == w_create && factory == w_factory, "GetProcAddress not wrapped");
#endif
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    CHECK(SUCCEEDED(create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx)), "device");
    IDXGIFactory *fac;
    CHECK(SUCCEEDED(factory(&iid_factory1, (void **)&fac)), "factory");
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"bptest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"bptest", L"", WS_POPUP, 0, 0, 320, 180, NULL, NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC scd = {{320, 180, {0, 1}, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 1,
                                hwnd, TRUE, DXGI_SWAP_EFFECT_DISCARD, 0};
    IDXGISwapChain *sc;
    CHECK(SUCCEEDED(IDXGIFactory_CreateSwapChain(fac, (IUnknown *)dev, &scd, &sc)), "swapchain");
#ifndef NOPROBE
    CHECK(g_sc == sc, "swapchain not seen");
#endif
    ID3D11Texture2D *bb, *scene, *ds;
    IDXGISwapChain_GetBuffer(sc, 0, &iid_tex2d, (void **)&bb);
    D3D11_TEXTURE2D_DESC td = {320, 180, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &scene);
    td.Format = DXGI_FORMAT_R24G8_TYPELESS; td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &ds);
    ID3D11RenderTargetView *bbv, *scv;
    ID3D11DepthStencilView *dsv;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &bbv);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)scene, NULL, &scv);
    D3D11_DEPTH_STENCIL_VIEW_DESC dvd = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D, 0, {{0}}};
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *)ds, &dvd, &dsv);
    D3D11_BUFFER_DESC bd = {560, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *cam, *other;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &cam);
    bd.ByteWidth = 64;
    ID3D11Device_CreateBuffer(dev, &bd, NULL, &other);
    // real shaders and an index buffer: draws with nothing bound hung the GPU on the test PC (device hung after a few frames)
    static const char hlsl[] = "float4 vs(uint i : SV_VertexID) : SV_Position { return float4(0.2 + 0.1 * (i & 1), 0.2 + 0.1 * (i >> 1), 0.5, 1); }\n"
                               "float4 ps() : SV_Target { return float4(0, 1, 0, 1); }\n";
    typedef HRESULT(WINAPI * comp_fn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
    comp_fn comp = (comp_fn)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    ID3DBlob *vsb, *psb;
    CHECK(comp && SUCCEEDED(comp(hlsl, sizeof hlsl - 1, "t", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, NULL)) &&
          SUCCEEDED(comp(hlsl, sizeof hlsl - 1, "t", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, NULL)), "shader compile");
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    unsigned short idx[3] = {0, 1, 2};
    D3D11_BUFFER_DESC ibd = {sizeof idx, D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA isd = {idx, 0, 0};
    ID3D11Buffer *ib;
    ID3D11Device_CreateBuffer(dev, &ibd, &isd, &ib);
    D3D11_VIEWPORT vp = {0, 0, 320, 180, 0, 1};

    for (int fr = 0; fr < 80; fr++) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);   // like the game's window
        if (fr == 10) {   // the user says "ready"
            wchar_t p[MAX_PATH];
            swprintf(p, MAX_PATH, L"%ls\\bridge_go.txt", dir);
            FILE *g = _wfopen(p, L"w");
            if (g) fclose(g);
        }
#ifndef NOPROBE
        if (fr == 5) {   // another mod hooks Draw and Present after the probe
            void **vt = *(void ***)ctx;
            f_d_orig = (d_fn)vt[SLOT(Draw)];
            patch_slot(&vt[SLOT(Draw)], (void *)f_d);
            vt = *(void ***)sc;
            void **sl = &vt[offsetof(IDXGISwapChainVtbl, Present) / sizeof(void *)];
            f_present_orig = *sl;
            patch_slot(sl, (void *)f_present);
        }
#endif
        D3D11_MAPPED_SUBRESOURCE m;
        float c[140] = {0};
        for (int i = 0; i < 16; i++) c[36 + i] = (float)(fr * 100 + i);   // view_projection at 144
        c[108] = 320; c[109] = 180;
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)cam, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, c, sizeof c);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)cam, 0);
        ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)other, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)other, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cam);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &scv, dsv);   // the 3D scene
        ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_DrawIndexed(ctx, 3, 0, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &bbv, NULL);   // the scene into the back buffer
        float red[4] = {1, 0, 0, 1};
        ID3D11DeviceContext_ClearRenderTargetView(ctx, bbv, red);
        ID3D11DeviceContext_Draw(ctx, 3, 0);
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &bbv, dsv);   // the UI on top
        ID3D11DeviceContext_DrawInstanced(ctx, 3, 1, 0, 0);
        IDXGISwapChain_Present(sc, 0, 0);
        HRESULT rr = ID3D11Device_GetDeviceRemovedReason(dev);
        CHECK(rr == S_OK, "device removed after frame %d: 0x%08lx", fr, rr);
    }

#ifndef NOPROBE
    // other mods' hooks: without the guards each of these recursed until the stack overflowed
    CHECK(f_d_calls && saw_left, "the other mod's Draw hook was not left alone (%u calls)", f_d_calls);
    real_present = r_present;
    r_present = loop_present;
    IDXGISwapChain_Present(sc, 0, 0);
    r_present = real_present;
    CHECK(g_nested == 1, "nested Present not caught (%u)", g_nested);
    HWND h2 = CreateWindowExW(0, L"bptest", L"", WS_POPUP, 0, 0, 320, 180, NULL, NULL, wc.hInstance, NULL);
    scd.OutputWindow = h2;
    IDXGISwapChain *sc2;   // a settings change: a new swapchain must not make the probe hook Present again
    CHECK(SUCCEEDED(IDXGIFactory_CreateSwapChain(fac, (IUnknown *)dev, &scd, &sc2)) && g_sc == sc2, "second swapchain");
    unsigned before = g_presents;
    CHECK(SUCCEEDED(IDXGISwapChain_Present(sc2, 0, 0)) && g_presents == before + 1 && g_nested == 1,
          "Present after the second swapchain (nested %u: hooked again over the other mod's hook)", g_nested);
#endif

    // the capture
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\bridge_capture_1.txt", dir);
    FILE *f = _wfopen(p, L"r");
    CHECK(f, "no capture file");
    static char txt[1 << 16];
    size_t n = fread(txt, 1, sizeof txt - 1, f);
    txt[n] = 0;
    fclose(f);
    fputs(txt, stdout);
    CHECK(strstr(txt, "swapchain 320x180 format 28 buffers 1 swap effect 0"), "swapchain description");
    CHECK(strstr(txt, "rt0 [BACKBUFFER 320x180 f28"), "back buffer target");
    CHECK(strstr(txt, "depth [320x180 f44"), "R24G8 depth");
    CHECK(strstr(txt, "clear depth [320x180 f44"), "depth clear");
    CHECK(strstr(txt, "back buffer dump 00 (1 draws into the back buffer"), "dump after the scene copy");
    CHECK(strstr(txt, "back buffer dump 01 (1 draws into the back buffer, at Present"), "dump at Present");
    CHECK(strstr(txt, "camera write map") && strstr(txt, "screen (320 180)"), "camera write");
    CHECK(strstr(txt, "VS cb0 = @") && strstr(txt, "(560 bytes)"), "VS cb0 bind");
    CHECK(strstr(txt, "end of captured frame: 4 draws, 3 target changes"), "draw / target counts");
    CHECK(strstr(txt, "(game +0x") && !strstr(txt, "(game +0x0 +0x0 +0x0)"), "game callers");
    swprintf(p, MAX_PATH, L"%ls\\bridge_bb_1_00.raw", dir);
    f = _wfopen(p, L"rb");
    CHECK(f, "no dump file");
    unsigned hdr[3], px;
    fread(hdr, 4, 3, f);
    fread(&px, 4, 1, f);
    fclose(f);
    CHECK(hdr[0] == 160 && hdr[1] == 90 && hdr[2] == 28 && px == 0xFF0000FFu, "dump content (%u %u %u %08x)", hdr[0], hdr[1], hdr[2], px);
    // camera records: 31 frames, each this frame's matrix (the 64-byte buffer is not taken)
    swprintf(p, MAX_PATH, L"%ls\\bridge_camera_1.bin", dir);
    f = _wfopen(p, L"rb");
    CHECK(f, "no camera file");
    unsigned rec[8], nrec = 0, bad = 0;
    float data[136];
    while (fread(rec, 4, 8, f) == 8 && fread(data, 1, 544, f) == 544) {
        nrec++;
        if (rec[0] != 0x43414D31u || rec[6] != 544 || rec[3] == 0 || data[36] != (float)(rec[1] * 100) || data[51] != data[36] + 15) bad++;
    }
    fclose(f);
    CHECK(nrec == 31 && !bad, "camera records %u, bad %u", nrec, bad);
    bp_report();
    printf("all bridge probe tests passed\n");
    return 0;
}
