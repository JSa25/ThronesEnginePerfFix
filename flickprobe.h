// Flicker probe (dev builds only; dlss/DLSS_LOG.md 11). The campaign map flickers below native size (Quality and lower,
// not DLAA or off), randomly and more while the camera moves; the terrain fix runs every frame and frame generation is
// not the cause. One run records everything needed to find it offline (tools/flick_analyze.py):
//  - a census of every target set the game uses: target 0 and depth (size, format), the game's call site, per mode
//    (scaled / native): frames, sets, draws, how often the glue shrank the viewport; at its first draw the pixel shader
//    and the screen-proportioned textures it reads (bridge_flick_census.txt);
//  - per frame: mode, scaled, render size, jitter, HUD-less point, camera, the census ids of the sets that ran, and the
//    CPU side: copies out of screen-sized textures, CPU reads (Map for reading), writes into large textures (streaming)
//    (bridge_flick_frames.bin);
//  - each frame as it goes out, small and grey (bridge_flick_img.bin; read back 3 frames late, no stall), so flicker
//    frames can be found by their pixels: a frame unlike both neighbours while they agree;
//  - the mode alternates by itself: 10 s scaled (the player's preset, Quality if not scaled) / 10 s DLAA, 6 periods.
// Armed by flicker_probe.txt in the game folder; starts once the campaign terrain pass has been drawn for 10 s; the
// player's mode is put back at the end. Included by bridgeprobe.c (its globals and hooks).

#define FP_W 320
#define FP_H 180
#define FP_MAXE 2048
typedef struct {
    const void *rt;           // target 0 (resource) or NULL
    UINT rw, rh, rf;          // its size / format
    UINT dw, dh;              // the depth's size (0: none)
    unsigned caller;          // the game's call site of the target change
    unsigned long long ps;    // pixel shader checksum at the first draw (last seen)
    char srv[160];            // screen-proportioned textures read at the first draw (first time)
    unsigned frames[2], sets[2], draws[2], shrunk[2], lastf[2];
} FpEntry;
static FpEntry g_fpe[FP_MAXE];
static unsigned g_nfpe;
static int g_fp_armed, g_fp_on, g_fp_done, g_fp_orig = -1, g_fp_mode;   // g_fp_mode: the scaled preset used
static unsigned g_fp_terrain_first = ~0u, g_fp_terrain_last = ~0u, g_fp_t0;
static unsigned g_fp_period_ms = 10000, g_fp_nperiods = 6, g_fp_delay_ms = 10000;
static int g_fp_test;   // tests: the clock counts frames (period / delay in frames), the mode is left alone
static int g_fp_cur = -1;                 // census entry of the target set bound now
static unsigned g_fp_set_draws;
static unsigned short g_fp_ids[512];
static unsigned g_fp_nids;
static unsigned g_fp_sets, g_fp_draws, g_fp_shrunk, g_fp_upd, g_fp_upd_bytes, g_fp_copies, g_fp_reads;
static FILE *g_fp_frames, *g_fp_img;
static unsigned g_fp_written, g_fp_imgs;
// read-back ring
static ID3D11Texture2D *g_fp_full, *g_fp_small, *g_fp_stage[4];
static ID3D11ShaderResourceView *g_fp_full_srv;
static ID3D11RenderTargetView *g_fp_small_rtv;
static ID3D11PixelShader *g_fp_ps;
static unsigned g_fp_stage_frame[4], g_fp_n;
static int g_fp_bad;

static unsigned fp_now(void) { return g_fp_test ? g_frame : GetTickCount(); }
static void fp_terrain(void) { g_fp_terrain_last = g_frame; }   // the campaign terrain pass drew this frame
static int fp_mode(void) { return g_frame_scaled && (g_rw != g_live_w || g_rh != g_live_h) ? 1 : 0; }

// the target set changes (live_targets, before it takes effect)
static void fp_set(ID3D11RenderTargetView *rtv0, ID3D11DepthStencilView *dsv) {
    if (!g_fp_on) return;
    FpEntry k = {0};
    ID3D11Resource *r = NULL;
    D3D11_RESOURCE_DIMENSION dim;
    D3D11_TEXTURE2D_DESC td;
    if (rtv0) {
        ID3D11RenderTargetView_GetResource(rtv0, &r);
        ID3D11Resource_GetType(r, &dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td), k.rw = td.Width, k.rh = td.Height, k.rf = td.Format;
        k.rt = r;
        ID3D11Resource_Release(r);
    }
    if (dsv) {
        ID3D11DepthStencilView_GetResource(dsv, &r);
        ID3D11Resource_GetType(r, &dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &td), k.dw = td.Width, k.dh = td.Height;
        ID3D11Resource_Release(r);
    }
    unsigned cs[3] = {0};
    game_callers((const unsigned *)__builtin_frame_address(0), cs, 3);
    k.caller = cs[1] ? cs[1] : cs[0];   // the first is the engine's target-change wrapper (+0x180F065 in every set)
    int e = -1;
    for (unsigned i = 0; i < g_nfpe && e < 0; i++)
        if (g_fpe[i].rt == k.rt && g_fpe[i].rw == k.rw && g_fpe[i].rh == k.rh && g_fpe[i].rf == k.rf && g_fpe[i].dw == k.dw &&
            g_fpe[i].dh == k.dh && g_fpe[i].caller == k.caller) e = (int)i;
    if (e < 0 && g_nfpe < FP_MAXE) g_fpe[e = (int)g_nfpe++] = k;
    g_fp_cur = e;
    g_fp_set_draws = 0;
    if (e < 0) return;
    int m = fp_mode();
    FpEntry *x = &g_fpe[e];
    x->sets[m]++;
    if (x->lastf[m] != g_frame + 1) x->lastf[m] = g_frame + 1, x->frames[m]++;
    if (g_fp_nids < 512) g_fp_ids[g_fp_nids++] = (unsigned short)e;
    g_fp_sets++;
}
// every draw of the game's context while on
static void fp_draw(ID3D11DeviceContext *c) {
    if (!g_fp_on || g_fp_cur < 0) return;
    FpEntry *x = &g_fpe[g_fp_cur];
    int m = fp_mode();
    x->draws[m]++;
    g_fp_draws++;
    if (g_fp_set_draws++) return;
    if (g_ngvp && g_avp[0].Width < g_gvp[0].Width) x->shrunk[m]++, g_fp_shrunk++;
    ID3D11PixelShader *ps = NULL;
    ID3D11DeviceContext_PSGetShader(c, &ps, NULL, NULL);
    for (LONG i = g_ncapps < 8192 ? g_ncapps : 8192; ps && i-- > 0;) if (g_capps[i].ps == ps) { x->ps = g_capps[i].sum; break; }
    if (ps) ID3D11PixelShader_Release(ps);
    if (x->srv[0]) return;
    ID3D11ShaderResourceView *s[16] = {0};
    ID3D11DeviceContext_PSGetShaderResources(c, 0, 16, s);
    size_t n = 0;
    for (int i = 0; i < 16; i++) {
        if (!s[i]) continue;
        ID3D11Resource *r = NULL;
        D3D11_RESOURCE_DIMENSION dim;
        ID3D11ShaderResourceView_GetResource(s[i], &r);
        ID3D11Resource_GetType(r, &dim);
        if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d;
            ID3D11Texture2D_GetDesc((ID3D11Texture2D *)r, &d);
            if (screen_part(d.Width, d.Height) && n < sizeof x->srv - 40)
                n += (size_t)snprintf(x->srv + n, sizeof x->srv - n, "t%d %ux%u f%u @%p ", i, d.Width, d.Height, d.Format, (void *)r);
        }
        ID3D11Resource_Release(r);
        ID3D11ShaderResourceView_Release(s[i]);
    }
    if (!x->srv[0]) snprintf(x->srv, sizeof x->srv, "-");
}
// copies (kind 0 whole, 1 part), updates (2), CPU reads (3)
static void fp_copy(int kind, ID3D11Resource *dst, ID3D11Resource *src, UINT bytes) {
    if (!g_fp_on) return;
    if (kind == 3) { g_fp_reads++; return; }
    if (src && big_tex(src)) g_fp_copies++;
    D3D11_RESOURCE_DIMENSION dim;
    ID3D11Resource_GetType(dst, &dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)dst, &d);
    if (d.Width >= 1024 && d.Height >= 1024 && !(d.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL))) g_fp_upd++, g_fp_upd_bytes += bytes;
}

static int fp_init_gpu(ID3D11Resource *bb) {
    if (!g_ssr_state && !ssr_init()) return 0;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)bb, &d);
    D3D11_TEXTURE2D_DESC f = d;
    f.Usage = D3D11_USAGE_DEFAULT; f.BindFlags = D3D11_BIND_SHADER_RESOURCE; f.CPUAccessFlags = 0; f.MiscFlags = 0; f.MipLevels = 1; f.ArraySize = 1;
    D3D11_TEXTURE2D_DESC s = {FP_W, FP_H, 1, 1, DXGI_FORMAT_R8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
    static const char src[] =
        "Texture2D<float4> t : register(t0);\n"
        "float ps(float4 p : SV_Position) : SV_Target {\n"
        "  uint w, h; t.GetDimensions(w, h); int2 b = int2(w / 320, h / 180); int2 o = int2(p.xy) * b; float s = 0;\n"
        "  for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) s += dot(t.Load(int3(o + int2(x, y) * b / 4, 0)).rgb, float3(0.299, 0.587, 0.114));\n"
        "  return s / 16; }\n";
    ssr_comp_fn comp = (ssr_comp_fn)(void *)GetProcAddress(LoadLibraryExA("d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), "D3DCompile");
    ID3DBlob *b = NULL;
    int ok = comp && SUCCEEDED(comp(src, sizeof src - 1, "fp", NULL, NULL, "ps", "ps_5_0", 0, 0, &b, NULL)) &&
             SUCCEEDED(ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &g_fp_ps)) &&
             SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &f, NULL, &g_fp_full)) &&
             SUCCEEDED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_fp_full, NULL, &g_fp_full_srv)) &&
             SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &s, NULL, &g_fp_small)) &&
             SUCCEEDED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_fp_small, NULL, &g_fp_small_rtv));
    s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; ok && i < 4; i++) ok = SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &s, NULL, &g_fp_stage[i]));
    if (b) ID3D10Blob_Release(b);
    return ok;
}
static void fp_write_img(int slot) {   // the staged picture of frame g_fp_stage_frame[slot] into the file
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ORIG(map_fn, S_MAP, g_ctx)(g_ctx, (ID3D11Resource *)g_fp_stage[slot], 0, D3D11_MAP_READ, 0, &m))) return;
    unsigned h[2] = {g_fp_stage_frame[slot], FP_W | FP_H << 16};
    fwrite(h, 4, 2, g_fp_img);
    for (int y = 0; y < FP_H; y++) fwrite((const unsigned char *)m.pData + (size_t)y * m.RowPitch, 1, FP_W, g_fp_img);
    ORIG(unmap_fn, S_UNMAP, g_ctx)(g_ctx, (ID3D11Resource *)g_fp_stage[slot], 0);
    g_fp_imgs++;
}
// at Present, before the frame goes to the helper: this frame's picture into the ring, the one 3 frames back to the file
static void fp_picture(void) {
    if (g_fp_bad) return;
    ID3D11Resource *bb = backbuffer();
    if (!bb) return;
    if (!g_fp_full && !fp_init_gpu(bb)) { g_fp_bad = 1; g_log("flicker probe: no picture read-back (shaders / textures) - pictures off"); ID3D11Resource_Release(bb); return; }
    r2m_internal = 1;
    ORIG(copyres_fn, S_COPYRES, g_ctx)(g_ctx, (ID3D11Resource *)g_fp_full, bb);
    ID3D11Resource_Release(bb);
    ID3DDeviceContextState *game = NULL;
    ID3D11DeviceContext1_SwapDeviceContextState(g_ssr_ctx1, g_ssr_state, &game);
    ID3D11DeviceContext *c = g_ctx;
    D3D11_VIEWPORT vp = {0, 0, FP_W, FP_H, 0, 1};
    ID3D11DeviceContext_OMSetRenderTargets(c, 1, &g_fp_small_rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(c, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(c, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(c, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(c, g_ssr_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(c, g_fp_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &g_fp_full_srv);
    ID3D11DeviceContext_Draw(c, 3, 0);
    ID3D11ShaderResourceView *none = NULL;
    ID3D11DeviceContext_PSSetShaderResources(c, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(c, 0, NULL, NULL);
    int slot = (int)(g_fp_n & 3);
    if (g_fp_n >= 4) fp_write_img(slot);   // (state swapped: these are our own staging textures)
    ID3D11DeviceContext_CopyResource(c, (ID3D11Resource *)g_fp_stage[slot], (ID3D11Resource *)g_fp_small);
    g_fp_stage_frame[slot] = g_frame;
    g_fp_n++;
    ID3D11DeviceContext1_SwapDeviceContextState(g_ssr_ctx1, game, NULL);
    if (game) ID3DDeviceContextState_Release(game);
    r2m_internal = 0;
    hook_context();
}
static void fp_open(void) {
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\bridge_flick_frames.bin", g_dir);
    g_fp_frames = _wfopen(p, L"wb");
    _snwprintf(p, MAX_PATH, L"%ls\\bridge_flick_img.bin", g_dir);
    g_fp_img = _wfopen(p, L"wb");
}
static void fp_census(void) {
    wchar_t p[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%ls\\bridge_flick_census.txt", g_dir);
    FILE *f = _wfopen(p, L"w");
    if (!f) return;
    fprintf(f, "# id | target 0 (size format @resource) | depth | game call site | pixel shader | native: frames sets draws shrunk | "
               "scaled: frames sets draws shrunk | screen-proportioned textures read at the first draw\n");
    for (unsigned i = 0; i < g_nfpe; i++) {
        FpEntry *x = &g_fpe[i];
        fprintf(f, "%u | %ux%u f%u @%p | %ux%u | +0x%X | %016llx | %u %u %u %u | %u %u %u %u | %s\n", i, x->rw, x->rh, x->rf, x->rt, x->dw,
                x->dh, x->caller, x->ps, x->frames[0], x->sets[0], x->draws[0], x->shrunk[0], x->frames[1], x->sets[1], x->draws[1],
                x->shrunk[1], x->srv[0] ? x->srv : "-");
    }
    fclose(f);
}
static void fp_stop(void) {
    for (unsigned k = g_fp_n >= 4 ? g_fp_n - 4 : 0; g_fp_img && !g_fp_bad && k < g_fp_n; k++) fp_write_img((int)(k & 3));   // the last 4 staged
    if (g_fp_frames) fclose(g_fp_frames), g_fp_frames = NULL;
    if (g_fp_img) fclose(g_fp_img), g_fp_img = NULL;
    fp_census();
    g_fp_on = 0;
    g_fp_done = 1;
    if (g_fp_orig >= 0 && g_fp_orig != g_sr && !g_fp_test) sr_set(g_fp_orig);
    g_log("flicker probe: done - %u frames, %u pictures, %u target sets in the census; super resolution back to %s", g_fp_written, g_fp_imgs,
          g_nfpe, SR_NAMES[g_sr]);
}
// at every Present (before the frame goes to the helper)
static void fp_present(void) {
    if (!g_fp_armed || g_fp_done) return;
    unsigned now = fp_now();
    if (!g_fp_on) {   // wait for the campaign terrain pass, then 10 s
        if (g_fp_terrain_last != g_frame) { g_fp_terrain_first = ~0u; return; }
        if (g_fp_terrain_first == ~0u) { g_fp_terrain_first = now; return; }
        if (now - g_fp_terrain_first < g_fp_delay_ms) return;
        g_fp_on = 1;
        g_fp_t0 = now;
        g_fp_orig = g_sr;
        g_fp_mode = g_sr >= 2 ? g_sr : 2;
        fp_open();
        g_log("flicker probe: started at frame %u - %u periods of %u %s, %s / DLAA by turns; keep moving the camera", g_frame,
              g_fp_nperiods, g_fp_test ? g_fp_period_ms : g_fp_period_ms / 1000, g_fp_test ? "frames" : "s", SR_NAMES[g_fp_mode]);
        if (g_sr != g_fp_mode && !g_fp_test) sr_set(g_fp_mode);
        g_fp_nids = g_fp_sets = g_fp_draws = g_fp_shrunk = g_fp_upd = g_fp_upd_bytes = g_fp_copies = g_fp_reads = 0;
        return;
    }
    // this frame's record
    if (g_fp_frames) {
        struct { unsigned frame, t; unsigned char sr, scaled, hud, cam; unsigned short rw, rh; float jx, jy, vp[8];
                 unsigned sets, draws, shrunk, upd, upd_bytes, copies, reads; unsigned short nids; } r;
        memset(&r, 0, sizeof r);
        r.frame = g_frame; r.t = now - g_fp_t0; r.sr = (unsigned char)g_sr; r.scaled = (unsigned char)fp_mode();
        r.hud = g_hud_frame == g_frame; r.cam = g_vp_frame == g_frame; r.rw = (unsigned short)g_rw; r.rh = (unsigned short)g_rh;
        r.jx = g_jx; r.jy = g_jy;
        memcpy(r.vp, g_vp, sizeof r.vp);
        r.sets = g_fp_sets; r.draws = g_fp_draws; r.shrunk = g_fp_shrunk; r.upd = g_fp_upd; r.upd_bytes = g_fp_upd_bytes;
        r.copies = g_fp_copies; r.reads = g_fp_reads; r.nids = (unsigned short)g_fp_nids;
        fwrite(&r, sizeof r, 1, g_fp_frames);
        fwrite(g_fp_ids, 2, g_fp_nids, g_fp_frames);
        g_fp_written++;
    }
    if (g_fp_img) fp_picture();
    g_fp_nids = g_fp_sets = g_fp_draws = g_fp_shrunk = g_fp_upd = g_fp_upd_bytes = g_fp_copies = g_fp_reads = 0;
    unsigned period = (now - g_fp_t0) / g_fp_period_ms;
    if (period >= g_fp_nperiods) { fp_stop(); return; }
    int want = period & 1 ? 1 : g_fp_mode;   // even periods scaled, odd DLAA
    if (g_sr != want && !g_fp_test) {
        sr_set(want);
        g_log("flicker probe: period %u, super resolution %s", period + 1, SR_NAMES[g_sr]);
    }
}
