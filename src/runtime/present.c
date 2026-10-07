/*
 * The presenter: our own window, showing the game's picture through Direct3D 11.
 *
 * The game keeps running exactly as it does headless (host.c): its display is
 * virtual (no mode change, the primary is a system-memory surface the host
 * owns) and its windows are real but invisible. This window is the only thing
 * the player sees. Each vsync it copies the primary (host_frame), uploads it to
 * a texture and draws it letterboxed into the client area through a scaling
 * shader; it maps the player's mouse back into game coordinates and posts it
 * to whichever game window is under it, and passes keys to the game's focus.
 * SimCity 2000's frontend has the same shape; the shader and device code are
 * Gunman's present_d3d.c. docs/presenter.md has the reasoning.
 *
 *   F11, Alt+Enter   borderless fullscreen on the window's monitor
 *   F12              scaling: sharp-bilinear (default), smooth, CRT, nearest, integer
 *   F10, right-click on the bars   the settings menu (scaling, bars,
 *                    fullscreen, the game's resolution); kept in ra2.ini
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "input.h"
#include "present.h"
#include "hdvox.h"
#include "recomp_target.h"

int host_frame(uint32_t* out, int maxw, int maxh, int* w, int* h);   /* host.c */
int host_frame_hd(uint32_t* out, int maxw, int maxh, int* w, int* h);   /* host.c: 2x, HD voxels */
HCURSOR host_menu_cursor(void);   /* host.c: the game's cursor, NULL while it draws its own */
HWND host_game_focus(void);       /* host.c: under Wine, the window the game gave the focus */
long host_frame_count(void);      /* host.c: blits into the primary so far */
int  host_under_wine(void);       /* host.c */

static const char* const k_mode_names[] = { "sharp", "smooth", "crt", "nearest", "integer" };
#define NMODES 5

typedef HRESULT (WINAPI *compile_fn)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

static const char k_hlsl[] =
"Texture2D t0 : register(t0);\n"
"SamplerState s_lin : register(s0);\n"
"SamplerState s_pt : register(s1);\n"
"cbuffer C : register(b0) { float2 src; float2 dst; int mode; int p0; int p1; int p2; };\n"
"struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) {\n"
"  V o; float2 p = float2((id << 1) & 2, id & 2);\n"
"  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = p; return o;\n"
"}\n"
"float3 sharp(float2 uv) {\n"                       /* sharp-bilinear */
"  float2 scale = max(dst / src, 1.0);\n"
"  float2 texel = uv * src, fl = floor(texel), c = frac(texel) - 0.5;\n"
"  float2 range = 0.5 - 0.5 / scale;\n"
"  float2 f = (c - clamp(c, -range, range)) * scale + 0.5;\n"
"  return t0.Sample(s_lin, (fl + f) / src).rgb;\n"
"}\n"
"float3 crt(float2 uv, float2 px) {\n"
"  float k = saturate((dst.y / src.y - 1.5) / 1.5);\n"  /* scanlines fade in from 1.5x to 3x (moire below) */
"  float3 col = sharp(uv);\n"
"  float d = frac(uv.y * src.y) - 0.5;\n"
"  float beam = lerp(1.0, exp(-d * d * 8.0) * 1.6, k);\n"
"  uint m = (uint)px.x % 3u;\n"
"  float3 mask = m == 0u ? float3(1.25, 0.87, 0.87) : m == 1u ? float3(0.87, 1.25, 0.87)\n"
"                                                  : float3(0.87, 0.87, 1.25);\n"
"  return col * beam * lerp(float3(1, 1, 1), mask, k);\n"
"}\n"
"float3 blur(float2 uv) {\n"                         /* the bars: a soft, dark copy of the picture */
"  float3 c = 0; float r = 0.035;\n"
"  [unroll] for (int y = -2; y <= 2; y++) [unroll] for (int x = -2; x <= 2; x++)\n"
"    c += t0.SampleLevel(s_lin, uv + float2(x, y) * r, 0).rgb;\n"
"  return c / 25.0 * 0.32;\n"
"}\n"
"float4 ps(V i) : SV_Target {\n"
"  float3 c;\n"
"  if (mode == 5) return float4(blur(i.uv), 1);\n"
"  if (mode == 0) c = sharp(i.uv);\n"
"  else if (mode == 1) c = t0.Sample(s_lin, i.uv).rgb;\n"
"  else if (mode == 2) c = crt(i.uv, i.pos.xy);\n"
"  else c = t0.Sample(s_pt, i.uv).rgb;\n"
"  return float4(saturate(c), 1);\n"
"}\n";

static struct {
    HWND hwnd;
    ID3D11Device* dev;
    ID3D11DeviceContext* ctx;
    IDXGISwapChain* sc;
    ID3D11RenderTargetView* rtv;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11SamplerState* samp[2];
    ID3D11Buffer* cb;
    ID3D11Texture2D* tex;
    ID3D11ShaderResourceView* srv;
    int tw, th;                 /* texture size */
    int cw, ch;                 /* swap chain size */
} d;

static int g_mode;                       /* index into k_mode_names */
static int g_bars = 1;                   /* the bars beside a letterboxed picture: 0 black, 1 blurred */
static volatile LONG g_gw = 800, g_gh = 600;   /* the game's picture, last frame */
static RECT g_dst;                       /* where it is drawn in the client area */
static int g_fullscreen;
static RECT g_windowed;                  /* the window's rect before fullscreen */
static HWND g_capture_target;            /* the game window a held button went to */
static HWND g_present_hwnd;              /* this window, for present_real_mouse */
#define WM_REAL_MOUSE (WM_APP + 0x10)    /* + (message - WM_MOUSEFIRST): lParam is a screen point */
static POINT g_capture_off;              /* game point -> that window's client */

/* ---- Direct3D 11 --------------------------------------------------------- */

static int d3d_init(HWND hw) {
    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_10_0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &fl, 1,
                                               D3D11_SDK_VERSION, &sd, &d.sc, &d.dev, NULL, &d.ctx);
    if (FAILED(hr))       /* no usable GPU: WARP is still a Direct3D pipeline */
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, &fl, 1,
                                           D3D11_SDK_VERSION, &sd, &d.sc, &d.dev, NULL, &d.ctx);
    if (FAILED(hr)) { fprintf(stderr, "[present] no Direct3D 11 device: 0x%08lX\n", hr); return 0; }
    IDXGIFactory* f = NULL;     /* Alt+Enter is ours (borderless), not DXGI's exclusive mode */
    if (SUCCEEDED(IDXGISwapChain_GetParent(d.sc, &IID_IDXGIFactory, (void**)&f))) {
        IDXGIFactory_MakeWindowAssociation(f, hw, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        IDXGIFactory_Release(f);
    }
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    compile_fn compile = dc ? (compile_fn)GetProcAddress(dc, "D3DCompile") : NULL;
    ID3DBlob *vb = NULL, *pb = NULL, *err = NULL;
    if (!compile) { fprintf(stderr, "[present] d3dcompiler_47.dll missing\n"); return 0; }
    hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "vs", "vs_4_0", 0, 0, &vb, &err);
    if (SUCCEEDED(hr))
        hr = compile(k_hlsl, sizeof k_hlsl - 1, "present", NULL, NULL, "ps", "ps_4_0", 0, 0, &pb, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[present] shader: %s\n", err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?");
        return 0;
    }
    ID3D11Device_CreateVertexShader(d.dev, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), NULL, &d.vs);
    ID3D11Device_CreatePixelShader(d.dev, ID3D10Blob_GetBufferPointer(pb), ID3D10Blob_GetBufferSize(pb), NULL, &d.ps);
    ID3D10Blob_Release(vb);
    ID3D10Blob_Release(pb);
    D3D11_SAMPLER_DESC s = {0};
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(d.dev, &s, &d.samp[0]);
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    ID3D11Device_CreateSamplerState(d.dev, &s, &d.samp[1]);
    D3D11_BUFFER_DESC b = {0};
    b.ByteWidth = 32;
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ID3D11Device_CreateBuffer(d.dev, &b, NULL, &d.cb);
    d.hwnd = hw;
    return 1;
}

/* Where a gw x gh picture goes in a cw x ch client area: centred, aspect kept,
 * and for "integer" a whole multiple (at least 1x). */
static RECT fit(int cw, int ch, int gw, int gh, int integer) {
    RECT r;
    int w, h;
    if (integer) {
        int k = cw / gw < ch / gh ? cw / gw : ch / gh;
        if (k < 1) k = 1;
        w = gw * k, h = gh * k;
    } else if ((long long)cw * gh > (long long)ch * gw) {
        h = ch, w = (int)((long long)ch * gw / gh);
    } else {
        w = cw, h = (int)((long long)cw * gh / gw);
    }
    r.left = (cw - w) / 2, r.top = (ch - h) / 2;
    r.right = r.left + w, r.bottom = r.top + h;
    return r;
}

static void draw(const uint32_t* frame, int gw, int gh) {
    RECT cr;
    GetClientRect(d.hwnd, &cr);
    int cw = cr.right > 0 ? cr.right : 1, ch = cr.bottom > 0 ? cr.bottom : 1;
    if (!d.tex || d.tw != gw || d.th != gh) {
        if (d.srv) ID3D11ShaderResourceView_Release(d.srv);
        if (d.tex) ID3D11Texture2D_Release(d.tex);
        D3D11_TEXTURE2D_DESC td = {0};
        td.Width = gw; td.Height = gh; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8X8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ID3D11Device_CreateTexture2D(d.dev, &td, NULL, &d.tex);
        ID3D11Device_CreateShaderResourceView(d.dev, (ID3D11Resource*)d.tex, NULL, &d.srv);
        d.tw = gw, d.th = gh;
        fprintf(stderr, "[present] picture %dx%d\n", gw, gh);
    }
    ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource*)d.tex, 0, NULL, frame, gw * 4, 0);
    if (!d.rtv || cw != d.cw || ch != d.ch) {
        if (d.rtv) { ID3D11RenderTargetView_Release(d.rtv); d.rtv = NULL; }
        ID3D11DeviceContext_OMSetRenderTargets(d.ctx, 0, NULL, NULL);
        if (FAILED(IDXGISwapChain_ResizeBuffers(d.sc, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0))) return;
        ID3D11Texture2D* back = NULL;
        IDXGISwapChain_GetBuffer(d.sc, 0, &IID_ID3D11Texture2D, (void**)&back);
        ID3D11Device_CreateRenderTargetView(d.dev, (ID3D11Resource*)back, NULL, &d.rtv);
        ID3D11Texture2D_Release(back);
        d.cw = cw, d.ch = ch;
    }
    RECT r = fit(cw, ch, gw, gh, g_mode == 4);
    g_dst = r;
    struct { float src[2], dst[2]; int mode, p0, p1, p2; } c = {
        { (float)gw, (float)gh }, { (float)(r.right - r.left), (float)(r.bottom - r.top) },
        g_mode == 4 ? 3 : g_mode, 0, 0, 0 };
    ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource*)d.cb, 0, NULL, &c, 0, 0);
    static const float black[4] = { 0, 0, 0, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(d.ctx, d.rtv, black);
    ID3D11DeviceContext_OMSetRenderTargets(d.ctx, 1, &d.rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(d.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(d.ctx, d.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(d.ctx, d.ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(d.ctx, 0, 1, &d.srv);
    ID3D11DeviceContext_PSSetSamplers(d.ctx, 0, 2, d.samp);
    ID3D11DeviceContext_PSSetConstantBuffers(d.ctx, 0, 1, &d.cb);
    /* The bars: the 4:3 menus in a 16:9 window leave two, and a soft dark copy
     * of the picture stretched over the whole client reads as a frame where
     * black reads as a hole. Drawn first; the picture goes on top. */
    if (g_bars && (r.left > 0 || r.top > 0)) {
        struct { float src[2], dst[2]; int mode, p0, p1, p2; } cb = {
            { (float)gw, (float)gh }, { (float)cw, (float)ch }, 5, 0, 0, 0 };
        D3D11_VIEWPORT full = { 0, 0, (float)cw, (float)ch, 0, 1 };
        ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource*)d.cb, 0, NULL, &cb, 0, 0);
        ID3D11DeviceContext_RSSetViewports(d.ctx, 1, &full);
        ID3D11DeviceContext_Draw(d.ctx, 3, 0);
        ID3D11DeviceContext_UpdateSubresource(d.ctx, (ID3D11Resource*)d.cb, 0, NULL, &c, 0, 0);
    }
    D3D11_VIEWPORT vp = { (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top), 0, 1 };
    ID3D11DeviceContext_RSSetViewports(d.ctx, 1, &vp);
    ID3D11DeviceContext_Draw(d.ctx, 3, 0);
    IDXGISwapChain_Present(d.sc, 1, 0);               /* vsync paces this thread */
}

/* ---- input ---------------------------------------------------------------- */

/* A client point of this window in game coordinates, clamped to the picture:
 * in the letterbox bars the cursor sits on the picture's edge, which is where
 * the game's edge scrolling expects a player at the screen's edge. */
static POINT to_game(int x, int y) {
    POINT p;
    int rw = g_dst.right - g_dst.left, rh = g_dst.bottom - g_dst.top;
    if (rw <= 0 || rh <= 0) { p.x = p.y = 0; return p; }
    p.x = (LONG)((long long)(x - g_dst.left) * g_gw / rw);
    p.y = (LONG)((long long)(y - g_dst.top) * g_gh / rh);
    if (p.x < 0) p.x = 0;
    if (p.y < 0) p.y = 0;
    if (p.x >= g_gw) p.x = g_gw - 1;
    if (p.y >= g_gh) p.y = g_gh - 1;
    return p;
}

/* The deepest visible game window at a game point, and the point in its client
 * coordinates. A static control passes its clicks to the dialog under it, as
 * Windows does (HTTRANSPARENT): the campaign screen's emblems are statics. */
/* Search state for target_at: every visible window of the game's thread,
 * deepest wins. The main menu's dialog is not a direct child of the main
 * window, so asking the main window which child is under the point found
 * nothing and the click went to the main window. */
static struct { POINT real; HWND best; int best_depth; } g_hit;

static int depth_of(HWND h) {
    int n = 0;
    while ((h = GetParent(h)) != NULL && n < 32) n++;
    return n;
}

static BOOL CALLBACK hit_child(HWND h, LPARAM unused) {
    RECT r;
    char cls[16] = "";
    (void)unused;
    if (!IsWindowVisible(h) || !IsWindowEnabled(h) || !GetWindowRect(h, &r) || !PtInRect(&r, g_hit.real))
        return TRUE;
    GetClassNameA(h, cls, sizeof cls);
    if (!_stricmp(cls, "Static")) return TRUE;
    int dep = depth_of(h);
    if (dep >= g_hit.best_depth) g_hit.best = h, g_hit.best_depth = dep;
    return TRUE;
}

static BOOL CALLBACK hit_top(HWND h, LPARAM unused) {
    hit_child(h, unused);
    EnumChildWindows(h, hit_child, 0);
    return TRUE;
}

/* This thread is DPI aware and the game's windows are not: asked from here,
 * their rectangles come back in physical pixels (2x at 200%) and no button
 * contained the point. So the hit test runs in the game windows' own,
 * unaware, context. */
typedef void* (WINAPI *set_dpi_ctx_t)(void*);
static set_dpi_ctx_t g_set_dpi_ctx;

static HWND target_at(POINT gp, POINT* local) {
    void* was = g_set_dpi_ctx ? g_set_dpi_ctx((void*)(intptr_t)-1) : NULL;   /* DPI_AWARENESS_CONTEXT_UNAWARE */
    HWND t = NULL;
    POINT o = { 0, 0 };
    if (!g_input_hwnd) goto done;
    ClientToScreen(g_input_hwnd, &o);              /* the virtual screen's (0,0), really */
    g_hit.real.x = gp.x + o.x, g_hit.real.y = gp.y + o.y;
    g_hit.best = NULL, g_hit.best_depth = -1;
    EnumThreadWindows(GetWindowThreadProcessId(g_input_hwnd, NULL), hit_top, 0);
    t = g_hit.best ? g_hit.best : g_input_hwnd;
    *local = g_hit.real;
    ScreenToClient(t, local);
done:
    if (was) g_set_dpi_ctx(was);
    return t;
}

static void forward_mouse(UINT m, WPARAM w, int x, int y) {
    POINT gp = to_game(x, y), local;
    HWND t;
    input_live_cursor(gp.x, gp.y);
    input_live_buttons((int)(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)));
    if (g_capture_target) {                        /* a held button: the window it went down on */
        t = g_capture_target;
        local.x = gp.x - g_capture_off.x, local.y = gp.y - g_capture_off.y;
    } else {
        t = target_at(gp, &local);
    }
    if (!t) return;
    if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN) {
        g_capture_target = t;
        g_capture_off.x = gp.x - local.x, g_capture_off.y = gp.y - local.y;
    }
    input_post_mouse(t, m, w, MAKELPARAM(local.x, local.y));
    if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) {
        char cls[32] = "";
        GetClassNameA(t, cls, sizeof cls);
        fprintf(stderr, "[present] click at game %ld,%ld -> %s %p (id %d) at %ld,%ld\n", gp.x, gp.y, cls,
                (void*)t, GetDlgCtrlID(t), local.x, local.y);
    }
    if ((m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP) &&
        !(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)))
        g_capture_target = NULL;
}

/* Keys go where the game's own thread has its keyboard focus: the main window
 * in a game, an edit box in a dialog. */
static void forward_key(UINT m, WPARAM w, LPARAM l) {
    GUITHREADINFO gti = { sizeof gti };
    HWND t = g_input_hwnd;
    if (host_game_focus())                         /* under Wine, where the game asked for it */
        t = host_game_focus();
    else if (GetGUIThreadInfo(GetWindowThreadProcessId(g_input_hwnd, NULL), &gti) && gti.hwndFocus)
        t = gti.hwndFocus;
    if (t) PostMessageA(t, m, w, l);
}

static void set_fullscreen(HWND hw, int on) {
    if (on == g_fullscreen) return;
    if (on) {
        MONITORINFO mi = { sizeof mi };
        GetWindowRect(hw, &g_windowed);
        GetMonitorInfoA(MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongA(hw, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hw, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongA(hw, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(hw, NULL, g_windowed.left, g_windowed.top, g_windowed.right - g_windowed.left,
                     g_windowed.bottom - g_windowed.top, SWP_FRAMECHANGED | SWP_NOZORDER);
    }
    g_fullscreen = on;
    fprintf(stderr, "[present] %s\n", on ? "fullscreen" : "windowed");
}

/* ---- settings ---------------------------------------------------------------
 * Kept in ra2.ini beside ra2.exe: [present] scale, bars, fullscreen and the
 * window's rect. The game's own settings stay in its RA2MD.INI. */
static char g_ini[MAX_PATH];

static void settings_path(void) {
    char* slash;
    GetModuleFileNameA(NULL, g_ini, MAX_PATH);
    slash = strrchr(g_ini, '\\');
    strcpy_s(slash ? slash + 1 : g_ini, MAX_PATH - (slash ? (slash + 1 - g_ini) : 0), "ra2.ini");
}

/* HD vehicles are off by default under Wine (CrossOver on a Mac): there they
 * cost about a quarter of the frame rate (62.5 frames a second without, 47
 * with, a skirmish at 1352x845) and drew artifacts (docs/voxels.md). Their
 * setting there is a key of its own, so a ra2.ini that a run before this
 * saved with hdvoxels=1 does not turn them back on, and Windows keeps its
 * default. */
static const char* hdvox_key(void) {
    return host_under_wine() ? "hdvoxels_wine" : "hdvoxels";
}

static void settings_save(HWND hw) {
    char v[64];
    WritePrivateProfileStringA("present", "scale", k_mode_names[g_mode], g_ini);
    WritePrivateProfileStringA("present", "bars", g_bars ? "blur" : "black", g_ini);
    WritePrivateProfileStringA("present", "fullscreen", g_fullscreen ? "1" : "0", g_ini);
    WritePrivateProfileStringA("present", hdvox_key(), ra2_vox_hd_on ? "1" : "0", g_ini);
    if (hw && !g_fullscreen && !IsIconic(hw)) {
        RECT r;
        GetWindowRect(hw, &r);
        _snprintf(v, sizeof v - 1, "%ld,%ld,%ld,%ld", r.left, r.top, r.right - r.left, r.bottom - r.top);
        v[sizeof v - 1] = 0;
        WritePrivateProfileStringA("present", "window", v, g_ini);
    }
}

/* The game's own resolution: the INI's [Video] for the next start, and the
 * options in memory (GameOptionsClass, the fields its "Resolution = %d X %d"
 * log line prints) so the next game opens at it and the game's own save on
 * exit does not put the old one back. The memory is only written while it
 * holds a plausible resolution. */
#ifdef RA2_TARGET_GAMEMD
#define GAME_OPTIONS 0x00A8EB60u
#define OPT_W_AT 0x24
#else
#define GAME_OPTIONS 0x00A40B18u      /* Red Alert 2: width at +0x20 */
#define OPT_W_AT 0x20
#endif
#define OPT_W ((volatile int32_t*)(uintptr_t)(GAME_OPTIONS + OPT_W_AT))
#define OPT_H ((volatile int32_t*)(uintptr_t)(GAME_OPTIONS + OPT_W_AT + 4))
static const int k_res[][2] = { { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1366, 768 }, { 1600, 900 },
                                { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
#define NRES ((int)(sizeof k_res / sizeof k_res[0]))

static int opt_ok(void) { return *OPT_W >= 320 && *OPT_W <= 8192 && *OPT_H >= 200 && *OPT_H <= 8192; }

static void set_game_resolution(int w, int h) {
    char path[MAX_PATH], v[16];
    GetFullPathNameA(RA2_INI_NAME, MAX_PATH, path, NULL);       /* the game runs in its folder */
    _snprintf(v, sizeof v - 1, "%d", w), v[sizeof v - 1] = 0;
    WritePrivateProfileStringA("Video", "ScreenWidth", v, path);
    _snprintf(v, sizeof v - 1, "%d", h), v[sizeof v - 1] = 0;
    WritePrivateProfileStringA("Video", "ScreenHeight", v, path);
    if (opt_ok()) *OPT_W = w, *OPT_H = h;
    fprintf(stderr, "[present] game resolution %dx%d (the next game opens at it)\n", w, h);
}

enum { ID_SCALE = 100, ID_BARS = 200, ID_FULL = 300, ID_RES = 400, ID_HDVOX = 500 };

static void settings_menu(HWND hw) {             /* at the mouse */
    POINT at;
    HMENU m = CreatePopupMenu(), sc = CreatePopupMenu(), bars = CreatePopupMenu(), res = CreatePopupMenu();
    static const char* const scale_label[NMODES] = { "Sharp (default)", "Smooth", "CRT", "Nearest", "Integer" };
    char lbl[32];
    for (int i = 0; i < NMODES; i++)
        AppendMenuA(sc, MF_STRING | (i == g_mode ? MF_CHECKED : 0), ID_SCALE + i, scale_label[i]);
    AppendMenuA(bars, MF_STRING | (g_bars ? MF_CHECKED : 0), ID_BARS + 1, "Blurred");
    AppendMenuA(bars, MF_STRING | (!g_bars ? MF_CHECKED : 0), ID_BARS + 0, "Black");
    for (int i = 0; i < NRES; i++) {
        _snprintf(lbl, sizeof lbl - 1, "%d x %d", k_res[i][0], k_res[i][1]), lbl[sizeof lbl - 1] = 0;
        AppendMenuA(res, MF_STRING | (opt_ok() && *OPT_W == k_res[i][0] && *OPT_H == k_res[i][1] ? MF_CHECKED : 0),
                    ID_RES + i, lbl);
    }
    AppendMenuA(m, MF_POPUP, (UINT_PTR)sc, "Scaling\tF12");
    AppendMenuA(m, MF_POPUP, (UINT_PTR)bars, "Bars beside the picture");
    AppendMenuA(m, MF_STRING | (g_fullscreen ? MF_CHECKED : 0), ID_FULL, "Fullscreen\tF11");
    AppendMenuA(m, MF_STRING | (ra2_vox_hd_on ? MF_CHECKED : 0), ID_HDVOX, "HD vehicles (voxels at 2x)");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_POPUP, (UINT_PTR)res, "Game resolution (next game)");
    GetCursorPos(&at);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, at.x, at.y, 0, hw, NULL);
    DestroyMenu(m);                                           /* and its submenus */
    if (cmd >= ID_SCALE && cmd < ID_SCALE + NMODES) g_mode = cmd - ID_SCALE;
    else if (cmd == ID_BARS || cmd == ID_BARS + 1) g_bars = cmd - ID_BARS;
    else if (cmd == ID_FULL) set_fullscreen(hw, !g_fullscreen);
    else if (cmd == ID_HDVOX) ra2_vox_hd_on = !ra2_vox_hd_on;      /* takes effect next frame */
    else if (cmd >= ID_RES && cmd < ID_RES + NRES) set_game_resolution(k_res[cmd - ID_RES][0], k_res[cmd - ID_RES][1]);
    if (cmd) settings_save(hw);
}

static DWORD WINAPI quit_soon(LPVOID unused) {
    (void)unused;
    Sleep(5000);                 /* the game had its chance to save its settings and go */
    ExitProcess(0);
    return 0;
}

/* Who shows the cursor changes when the game captures or releases the mouse
 * (a battle starts or ends), without the mouse moving: WM_SETCURSOR alone
 * would leave the menus' arrow over the battlefield, beside the game's own,
 * until the player moved. */
static void update_cursor(HWND hw) {
    static HCURSOR was = (HCURSOR)-1;
    HCURSOR c = host_menu_cursor();
    POINT p;
    if (c == was) return;
    fprintf(stderr, "[present] cursor: %s\n", c ? "the game's Windows cursor (menus)" : "drawn by the game");
    was = c;
    if (GetCursorPos(&p) && WindowFromPoint(p) == hw &&
        SendMessageA(hw, WM_NCHITTEST, 0, MAKELPARAM(p.x, p.y)) == HTCLIENT)
        SetCursor(c);
}

static LRESULT CALLBACK wndproc(HWND hw, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        /* A right-click on the bars, outside the picture, opens the settings;
         * on the picture it is the game's (deselect, scroll). */
        if ((m == WM_RBUTTONDOWN || m == WM_RBUTTONUP) && !g_capture_target) {
            POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            if (!PtInRect(&g_dst, p)) {
                if (m == WM_RBUTTONUP) settings_menu(hw);
                return 0;
            }
        }
        if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN) SetCapture(hw);
        if ((m == WM_LBUTTONUP || m == WM_RBUTTONUP || m == WM_MBUTTONUP) &&
            !(w & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)))
            ReleaseCapture();
        forward_mouse(m == WM_LBUTTONDBLCLK ? WM_LBUTTONDOWN : m == WM_RBUTTONDBLCLK ? WM_RBUTTONDOWN : m,
                      w, GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_MOUSEWHEEL:
        forward_key(m, w, l);
        return 0;
    case WM_REAL_MOUSE + (WM_MOUSEMOVE - WM_MOUSEFIRST): case WM_REAL_MOUSE + (WM_LBUTTONDOWN - WM_MOUSEFIRST):
    case WM_REAL_MOUSE + (WM_LBUTTONUP - WM_MOUSEFIRST): case WM_REAL_MOUSE + (WM_LBUTTONDBLCLK - WM_MOUSEFIRST):
    case WM_REAL_MOUSE + (WM_RBUTTONDOWN - WM_MOUSEFIRST): case WM_REAL_MOUSE + (WM_RBUTTONUP - WM_MOUSEFIRST):
    case WM_REAL_MOUSE + (WM_RBUTTONDBLCLK - WM_MOUSEFIRST): case WM_REAL_MOUSE + (WM_MBUTTONDOWN - WM_MOUSEFIRST):
    case WM_REAL_MOUSE + (WM_MBUTTONUP - WM_MOUSEFIRST): {
        POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };   /* present_real_mouse: a screen point */
        ScreenToClient(hw, &p);
        return wndproc(hw, WM_MOUSEFIRST + (m - WM_REAL_MOUSE), w, MAKELPARAM(p.x, p.y));
    }
    case WM_SETCURSOR:
        /* In a battle the game draws its own; in the menus, the Windows
         * cursor it set (update_cursor). */
        if (LOWORD(l) == HTCLIENT) { SetCursor(host_menu_cursor()); return TRUE; }
        break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN) { set_fullscreen(hw, !g_fullscreen); settings_save(hw); return 0; }
        if (w == VK_F10) { settings_menu(hw); return 0; }   /* F10 arrives as a system key */
        forward_key(m, w, l);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_F11) { set_fullscreen(hw, !g_fullscreen); settings_save(hw); return 0; }
        if (w == VK_F12) {
            g_mode = (g_mode + 1) % NMODES;
            fprintf(stderr, "[present] scaling: %s\n", k_mode_names[g_mode]);
            settings_save(hw);
            return 0;
        }
        forward_key(m, w, l);
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
        if (w == VK_F10 || w == VK_F11 || w == VK_F12) return 0;
        forward_key(m, w, l);
        return 0;
    case WM_CLOSE:
        settings_save(hw);
        if (g_input_hwnd) PostMessageA(g_input_hwnd, WM_CLOSE, 0, 0);
        CloseHandle(CreateThread(NULL, 0, quit_soon, NULL, 0, NULL));
        return 0;
    }
    return DefWindowProcA(hw, m, w, l);
}

/* RA2_FRAME_STATS=1: every 2 s, how many pictures the presenter showed, what
 * the copy out of the primary and the draw (upload, shader, Present) took,
 * and how many frames the game itself ran, so a slow picture says whether
 * the game or the presenter is slow. */
static void frame_stats(LARGE_INTEGER t0, LARGE_INTEGER t1, LARGE_INTEGER t2) {
    static int on = -1, n;
    static LARGE_INTEGER hz, start;
    static long long copy, drawn;
    static uint32_t game0;
    if (on < 0) on = getenv("RA2_FRAME_STATS") != NULL, QueryPerformanceFrequency(&hz), start = t0;
    if (!on) return;
    uint32_t game = *(volatile uint32_t*)(uintptr_t)RA2_GAME_FRAME_VA;
    copy += t1.QuadPart - t0.QuadPart, drawn += t2.QuadPart - t1.QuadPart, n++;
    if (t2.QuadPart - start.QuadPart < 2 * hz.QuadPart) return;
    double s = (double)(t2.QuadPart - start.QuadPart) / hz.QuadPart;
    /* the game's count starts again with each game: none counted across that */
    fprintf(stderr, "[present] %.1f pictures/s (copy %.1f ms, draw %.1f ms), game %.1f frames/s\n", n / s,
            1000.0 * copy / hz.QuadPart / n, 1000.0 * drawn / hz.QuadPart / n, game >= game0 ? (game - game0) / s : 0.0);
    game0 = game, start = t2, copy = drawn = 0, n = 0;
}


static DWORD WINAPI present_thread(LPVOID arg) {
    static uint32_t frame[4096 * 2160];
    int fullscreen = (int)(intptr_t)arg;
    /* Per-monitor DPI aware, this thread only. Unaware, Windows stretched the
     * window's pixels by the display scale (2x at 200%) after our shader had
     * scaled them, and sharp-bilinear came out blurred. The game's own windows
     * stay unaware: nothing about how the game sees its screen changes. */
    {
        g_set_dpi_ctx = (set_dpi_ctx_t)GetProcAddress(GetModuleHandleA("user32.dll"),
                                                      "SetThreadDpiAwarenessContext");
        if (g_set_dpi_ctx) g_set_dpi_ctx((void*)(intptr_t)-4);   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    }
    WNDCLASSA wc = {0};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
    wc.lpszClassName = "RA2Presenter";
    RegisterClassA(&wc);

    /* 4:3 at 85% of the work area's height: sharp-bilinear keeps whole texels
     * crisp at any scale, so there is no need to stop at a whole multiple (which
     * on a 1080p screen is 1x, a small window). */
    RECT wa, r = { 0, 0, 0, 0 };
    char saved[64] = "";
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
    r.bottom = (wa.bottom - wa.top) * 85 / 100;
    r.right = r.bottom * 4 / 3;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    int wx = wa.left + ((wa.right - wa.left) - ww) / 2, wy = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    GetPrivateProfileStringA("present", "window", "", saved, sizeof saved, g_ini);
    {
        int sx, sy, sw, sh;
        if (sscanf(saved, "%d,%d,%d,%d", &sx, &sy, &sw, &sh) == 4 && sw > 200 && sh > 150) {
            RECT want = { sx, sy, sx + sw, sy + sh };
            if (MonitorFromRect(&want, MONITOR_DEFAULTTONULL))           /* still on a screen */
                wx = sx, wy = sy, ww = sw, wh = sh;
        }
    }
    HWND hw = CreateWindowExA(0, "RA2Presenter", RA2_TITLE " (recomp)", WS_OVERLAPPEDWINDOW,
                              wx, wy, ww, wh, NULL, NULL, wc.hInstance, NULL);
    g_present_hwnd = hw;
    if (!hw || !d3d_init(hw)) {
        fprintf(stderr, "[present] could not start; run with --classic for the original display\n");
        ExitProcess(5);
    }
    ShowWindow(hw, SW_SHOW);
    SetForegroundWindow(hw);
    if (fullscreen) set_fullscreen(hw, 1);
    GetClientRect(hw, &r);
    fprintf(stderr, "[present] Direct3D 11 presenter, %ldx%ld window, scaling %s (F12), fullscreen F11, settings F10\n",
            r.right, r.bottom, k_mode_names[g_mode]);
    for (;;) {
        MSG msg;
        int gw, gh;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        update_cursor(hw);
        /* The copy holds the primary's lock (4.6 ms at 1352x845 on a Mac), and
         * the game waits on that lock to draw: copying as fast as the loop ran
         * (97 a second, where vsync did not hold it back) kept the game at half
         * its speed. So a copy is made when the game has blitted a new frame,
         * and at least every 33 ms besides, for what Bink writes straight into
         * the primary (movies). */
        {
            static long seen = -1;
            static DWORD last;
            long n = host_frame_count();
            DWORD since = GetTickCount() - last;
            /* a battle copies into its frame surface more than once a frame:
             * at most one picture per 16 ms, a display's refresh */
            if ((n == seen && since < 33) || since < 16) { Sleep(1); continue; }
            seen = n, last = GetTickCount();
        }
        LARGE_INTEGER t0, t1, t2;
        QueryPerformanceCounter(&t0);
        if (host_frame_hd(frame, 4096, 2160, &gw, &gh)) {     /* the picture at 2x */
            InterlockedExchange(&g_gw, gw);
            InterlockedExchange(&g_gh, gh);
            QueryPerformanceCounter(&t1);
            draw(frame, 2 * gw, 2 * gh);
            QueryPerformanceCounter(&t2);
            frame_stats(t0, t1, t2);
        } else if (host_frame(frame, 4096, 2160, &gw, &gh)) {
            InterlockedExchange(&g_gw, gw);
            InterlockedExchange(&g_gh, gh);
            QueryPerformanceCounter(&t1);
            draw(frame, gw, gh);
            QueryPerformanceCounter(&t2);
            frame_stats(t0, t1, t2);
        } else {
            Sleep(16);
        }
    }
}

void present_real_mouse(UINT m, WPARAM w, POINT screen) {
    if (g_present_hwnd) PostMessageA(g_present_hwnd, WM_REAL_MOUSE + (m - WM_MOUSEFIRST), w, MAKELPARAM(screen.x, screen.y));
}

int present_mode_from_name(const char* name) {
    for (int i = 0; i < NMODES; i++)
        if (!_stricmp(name, k_mode_names[i])) return i;
    return -1;
}

void present_start(int mode, int fullscreen) {
    char v[16];
    settings_path();
    /* ra2.ini first; a choice on the command line (mode or fullscreen >= 0) wins. */
    GetPrivateProfileStringA("present", "scale", "sharp", v, sizeof v, g_ini);
    g_mode = present_mode_from_name(v) >= 0 ? present_mode_from_name(v) : 0;
    GetPrivateProfileStringA("present", "bars", "blur", v, sizeof v, g_ini);
    g_bars = _stricmp(v, "black") != 0;
    if (fullscreen < 0) fullscreen = GetPrivateProfileIntA("present", "fullscreen", 0, g_ini);
    if (!ra2_vox_hd_on)                     /* --hd-voxels forces it */
        ra2_vox_hd_on = GetPrivateProfileIntA("present", hdvox_key(), !host_under_wine(), g_ini);
    if (mode >= 0) g_mode = mode;
    input_live(1);
    CloseHandle(CreateThread(NULL, 0, present_thread, (LPVOID)(intptr_t)fullscreen, 0, NULL));
}
