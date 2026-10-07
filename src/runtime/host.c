/*
 * Red Alert 2: Yuri's Revenge - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and machine lock; see its header). What is here is only what is specific to
 * this game: where the image goes, the command line, headless DirectDraw, and
 * the fault report. docs/host.md has the reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so 0x00400000..0x00B7A000 is
 * free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddraw.h>

#include "native32.h"
#include "recomp_trace.h"
#include "input.h"
#include "oracle.h"
#include "present.h"
#include "hdvox.h"
#include "recomp_target.h"   /* the binary this build runs: run_lift.py --target */

extern const uint32_t ra2_entry_va;    /* recomp_dispatch.c */

#define RA2_IMAGE_BASE 0x00400000u
/* A NULL module is the process exe, which is the host; mean the guest. */
#define GUEST_MODULE(h) ((h) ? (h) : RA2_IMAGE_BASE)
static int g_last_dialog;             /* resource ID of the last RT_DIALOG looked up */

static DWORD g_watchdog_s;
static int   g_original;            /* --original: run the shipping code (oracle.c) */
static int   g_headless;
/* How the game is shown. The presenter (present.c) is the default: the display
 * is virtual, as headless, and our own Direct3D 11 window shows it. --classic is
 * the game's own exclusive-fullscreen DirectDraw, as it shipped. */
static int   g_classic, g_fullscreen = -1, g_scale_mode = -1;   /* -1: as ra2.ini says */

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
static const char* gstr(uint32_t va) { return va ? (const char*)(uintptr_t)va : "(null)"; }

/* ---- headless ------------------------------------------------------------
 * Nothing reaches the screen. Over RDP a window lands on whatever device is
 * connected and a display-mode change resizes it (REPO_RULES section 13), so
 * message boxes print, the window is created hidden, and DirectDraw is kept
 * from going fullscreen: see "headless DirectDraw" below. */

static uint32_t mb_answer(uint32_t type) {
    uint32_t buttons = type & MB_TYPEMASK;
    return (buttons == MB_YESNO || buttons == MB_YESNOCANCEL) ? IDNO : IDOK;
}

static HWND g_game_hwnd;
static DWORD g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;
static int g_mode_set;

static void shim_MessageBoxA(void) {
    g_eax = mb_answer(ARG(3));
    fprintf(stderr, "[messagebox] type 0x%X -> %u: %s: %s\n", ARG(3), g_eax, gstr(ARG(2)), gstr(ARG(1)));
    g_esp += 4 + 4 * 4;
}

/* The game's windows are real but never on any screen. A hidden window gets
 * no WM_PAINT, and RA2's menus are dialogs whose controls the game paints
 * itself: hidden, the main menu showed its frame and no buttons (bringup.md,
 * 10). So a top-level window is layered at alpha 0, click-through, never
 * activated and kept off the taskbar: Windows treats it as visible and paints
 * it, and nothing appears on the screen or takes over an RDP session. */
#define HL_EXSTYLE (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)

/* Under Wine (CrossOver on a Mac) WS_EX_NOACTIVATE is not enough. A Button
 * the presenter clicks calls SetFocus, Wine activates its invisible top-level
 * window, and the Mac makes that the key window: the presenter went inactive
 * (the Mac's own cursor came back), the button's SetCapture took the real
 * mouse, and the button-up arrived at its real place on the screen, outside
 * it, so no click ever completed. Windows never moves the foreground for a
 * background thread's SetFocus. So under Wine, with the presenter, a CBT hook
 * on the game's thread refuses that activation, and remembers where the game
 * asked for the focus: that is where the presenter sends keys
 * (host_game_focus). */
static HHOOK g_cbt;
static volatile HWND g_game_focus;

static LRESULT CALLBACK cbt_hook(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_SETFOCUS) g_game_focus = (HWND)w;
    else if (code == HCBT_ACTIVATE) return 1;      /* refused: the presenter stays in front */
    return CallNextHookEx(g_cbt, code, w, l);
}

/* And the mouse. A Button takes the capture on the press the presenter
 * forwards, and Wine then sends the real mouse to it, in coordinates of its
 * real place on the screen: the button-up came at 603,166 to a 150x40
 * button, so the press was cancelled, and the presenter never saw the
 * release. On the game's thread, a mouse message nobody in the host posted
 * (input_post_mouse) is the real mouse: it is dropped here and handed to the
 * presenter at its screen point, which forwards it like any other. */
static LRESULT CALLBACK wine_mouse_hook(int code, WPARAM w, LPARAM l) {
    MSG* m = (MSG*)l;
    if (code == HC_ACTION && m->message >= WM_MOUSEFIRST && m->message <= WM_MOUSELAST &&
        m->message != WM_MOUSEWHEEL) {
        if (w == PM_REMOVE && !input_posted_mouse(m->hwnd, m->message, m->lParam)) {
            present_real_mouse(m->message, m->wParam, m->pt);
            m->message = WM_NULL;
        } else if (w != PM_REMOVE && !input_posted_mouse_peek(m->hwnd, m->message, m->lParam)) {
            m->message = WM_NULL;                  /* a peek at it sees nothing either */
        }
    }
    return CallNextHookEx(NULL, code, w, l);
}

static int under_wine(void) {
    return GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") != NULL;
}

int host_under_wine(void) { return under_wine(); }   /* present.c */

/* The game window keys go to: where it last asked for the focus under Wine,
 * else NULL (ask Windows). */
HWND host_game_focus(void) {
    HWND f = g_game_focus;
    return f && IsWindow(f) && IsWindowVisible(f) ? f : NULL;
}

/* The game also brings its window forward itself. Under Wine that moved the
 * foreground to the game's thread before the hook could refuse the
 * activation: then nothing was in front, and the button's capture had the
 * real mouse after all. So there these are answered without the call: the
 * game believes its window is in front, as the focus queries already say. */
static void shim_SetForegroundWindow(void) {
    g_eax = g_cbt ? TRUE : (uint32_t)SetForegroundWindow((HWND)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}

static void shim_BringWindowToTop(void) {
    g_eax = g_cbt ? TRUE : (uint32_t)BringWindowToTop((HWND)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}

static void shim_SetActiveWindow(void) {
    g_eax = g_cbt ? (uint32_t)(uintptr_t)g_game_hwnd
                  : (uint32_t)(uintptr_t)SetActiveWindow((HWND)(uintptr_t)ARG(0));
    g_esp += 4 + 1 * 4;
}

static void shim_CreateWindowExA(void) {
    uint32_t style = ARG(3);
    if (!g_cbt && !g_headless && !g_classic && under_wine()) {
        g_cbt = SetWindowsHookExA(WH_CBT, cbt_hook, NULL, GetCurrentThreadId());
        SetWindowsHookExA(WH_GETMESSAGE, wine_mouse_hook, NULL, GetCurrentThreadId());
        fprintf(stderr, "[headless] under Wine: game windows are never activated (CBT hook %s)\n",
                g_cbt ? "on" : "FAILED");
    }
    HWND parent = (HWND)(uintptr_t)ARG(8);
    int top = !parent || !(style & WS_CHILD);
    HWND h = CreateWindowExA(ARG(0) | (top ? HL_EXSTYLE : 0), (LPCSTR)(uintptr_t)ARG(1),
                             (LPCSTR)(uintptr_t)ARG(2), style & ~WS_VISIBLE, (int)ARG(4), (int)ARG(5),
                             (int)ARG(6), (int)ARG(7), parent, (HMENU)(uintptr_t)ARG(9),
                             (HINSTANCE)(uintptr_t)ARG(10), (LPVOID)(uintptr_t)ARG(11));
    if (h && top) SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
    if (h && (style & WS_VISIBLE)) ShowWindow(h, SW_SHOWNOACTIVATE);
    fprintf(stderr, "[headless] CreateWindowExA(\"%s\", %dx%d) from sub_%08X -> invisible hwnd %p\n",
            gstr(ARG(2)), (int)ARG(6), (int)ARG(7), g_cur_func, (void*)h);
    if (!g_game_hwnd) g_game_hwnd = g_input_hwnd = h;
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

/* ---- the virtual screen ---------------------------------------------------
 * The game was written for an exclusive fullscreen window, whose client area
 * IS the screen: it offsets drawing into the primary by window positions (the
 * Bink intro, 0x00432EF0; the menus' button captions). With a virtual display
 * the game's windows sit wherever Windows, or offstage, puts them -- offstage
 * moves every window of a program onto its monitor, and the captions then
 * landed off the surface. So every screen coordinate the game sees is
 * relative to its main window's client corner: that corner is the virtual
 * screen's (0,0), as it was in fullscreen, wherever it really is. */
static POINT vorigin(void) {
    POINT o = { 0, 0 };
    if (g_game_hwnd) ClientToScreen(g_game_hwnd, &o);
    return o;
}

static void shim_ClientToScreen(void) {
    POINT* p = (POINT*)(uintptr_t)ARG(1);
    POINT o = vorigin();
    g_eax = (uint32_t)ClientToScreen((HWND)(uintptr_t)ARG(0), p);
    p->x -= o.x, p->y -= o.y;
    g_esp += 4 + 2 * 4;
}

static void shim_ScreenToClient(void) {
    POINT* p = (POINT*)(uintptr_t)ARG(1);
    POINT o = vorigin();
    p->x += o.x, p->y += o.y;
    g_eax = (uint32_t)ScreenToClient((HWND)(uintptr_t)ARG(0), p);
    g_esp += 4 + 2 * 4;
}

static void shim_GetWindowRect(void) {
    RECT* r = (RECT*)(uintptr_t)ARG(1);
    POINT o = vorigin();
    g_eax = (uint32_t)GetWindowRect((HWND)(uintptr_t)ARG(0), r);
    OffsetRect(r, -o.x, -o.y);
    g_esp += 4 + 2 * 4;
}

/* SendMessage answers in screen coordinates too, where a message does: the
 * dialogs lay a combo box out by its CB_GETDROPPEDCONTROLRECT (0x00775BC0).
 * On a Mac the game's main window cannot sit at the top of the screen (the
 * menu bar), so its client corner was 33 pixels down and every combo box
 * moved down by that much: the skirmish screen's Side, Color, Start and Team
 * boxes sat a row below their players. */
static void shim_SendMessageA(void) {
    UINT m = ARG(1);
    g_eax = (uint32_t)SendMessageA((HWND)(uintptr_t)ARG(0), m, (WPARAM)ARG(2), (LPARAM)ARG(3));
    if (m == CB_GETDROPPEDCONTROLRECT && ARG(3)) {
        POINT o = vorigin();
        OffsetRect((RECT*)(uintptr_t)ARG(3), -o.x, -o.y);
    }
    g_esp += 4 + 4 * 4;
}

static void shim_WindowFromPoint(void) {
    POINT o = vorigin(), p = { (LONG)ARG(0) + o.x, (LONG)ARG(1) + o.y };
    g_eax = (uint32_t)(uintptr_t)WindowFromPoint(p);
    g_esp += 4 + 2 * 4;
}

/* A top-level window is placed in screen coordinates, a child in its parent's
 * client coordinates (left alone). The main window stays where it is: it
 * defines the origin; only its size is the game's. */
static int top_level(HWND h) { return !(GetWindowLongA(h, GWL_STYLE) & WS_CHILD); }

static void shim_MoveWindow(void) {
    HWND h = (HWND)(uintptr_t)ARG(0);
    int x = (int)ARG(1), y = (int)ARG(2), w = (int)ARG(3), hh = (int)ARG(4);
    if (h == g_game_hwnd) {
        RECT r;
        GetWindowRect(h, &r);
        x = r.left, y = r.top;
    } else if (top_level(h)) {
        POINT o = vorigin();
        x += o.x, y += o.y;
    }
    g_eax = (uint32_t)MoveWindow(h, x, y, w, hh, (BOOL)ARG(5));
    g_esp += 4 + 6 * 4;
}

static void shim_SetWindowPos(void) {
    HWND h = (HWND)(uintptr_t)ARG(0);
    int x = (int)ARG(2), y = (int)ARG(3);
    UINT flags = ARG(6);
    if (!(flags & SWP_NOMOVE)) {
        if (h == g_game_hwnd) flags |= SWP_NOMOVE;
        else if (top_level(h)) { POINT o = vorigin(); x += o.x, y += o.y; }
    }
    g_eax = (uint32_t)SetWindowPos(h, (HWND)(uintptr_t)ARG(1), x, y, (int)ARG(4), (int)ARG(5), flags);
    g_esp += 4 + 7 * 4;
}

/* The game parks or warps its cursor; only the virtual one moves. The real
 * cursor belongs to the player (and over RDP, to a phone). */
static void shim_SetCursorPos(void) {
    input_live_cursor((int)ARG(0), (int)ARG(1));
    g_eax = TRUE;
    g_esp += 4 + 2 * 4;
}

/* The cursor. The menus are Win32 dialogs and use the Windows cursor: the
 * game loads its own arrow (resource 0x68) and sets it. In a battle the mouse
 * is captured (Capture_Mouse, its +0x10 byte) and the game draws the cursor
 * into the picture itself. The presenter hides the real cursor over the
 * picture, so while the mouse is not captured it shows the one the game set,
 * and the menus had no cursor at all without it. */
#ifdef RA2_TARGET_GAME
#define RA2_MOUSE_VA 0x00B2AF5Cu   /* the mouse object (WWMouseClass), set by its constructor */
#else
#define RA2_MOUSE_VA 0x00B78164u
#endif
static HCURSOR g_game_cursor;

static void shim_SetCursor(void) {
    HCURSOR c = (HCURSOR)(uintptr_t)ARG(0);
    if (c) g_game_cursor = c;
    g_eax = (uint32_t)(uintptr_t)SetCursor(c);
    g_esp += 4 + 1 * 4;
}

/* The cursor the presenter shows over the picture: NULL while the game draws
 * its own. */
HCURSOR host_menu_cursor(void) {
    uint32_t mouse = MEM32(RA2_MOUSE_VA);
    if (mouse && MEM8(mouse + 0x10)) return NULL;
    return g_game_cursor ? g_game_cursor : LoadCursor(NULL, IDC_ARROW);
}

/* ...and the screen it reports is the mode's, as it would be after a real
 * mode change. Before the first SetDisplayMode it is the real desktop. */
static void shim_GetSystemMetrics(void) {
    int i = (int)ARG(0);
    g_eax = (uint32_t)GetSystemMetrics(i);
    if (g_mode_set && i == SM_CXSCREEN) g_eax = g_mode_w;
    if (g_mode_set && i == SM_CYSCREEN) g_eax = g_mode_h;
    g_esp += 4 + 1 * 4;
}

/* Focus. The window procedure keeps GameInFocus (0x00A8ED80) from the wParam
 * of activation messages, and the movie player calls BinkPause(1) while it is
 * clear: a hidden window is never the foreground one, so whenever Windows
 * said so the movie froze for good, at a moment that depended on timing
 * (bringup.md, 7). Headless, the game's window is always the active one, as
 * a fullscreen game in front is: its window procedure is wrapped to see every
 * activation message as "active", and the focus queries answer its window. */
#define MAX_CLASSES 8
static struct { ATOM atom; uint32_t proc; } g_classes[MAX_CLASSES];
static int g_nclasses;

static LRESULT CALLBACK hl_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    ATOM atom = (ATOM)GetClassLongA(h, GCW_ATOM);
    uint32_t proc = 0;
    for (int i = 0; i < g_nclasses; i++)
        if (g_classes[i].atom == atom) proc = g_classes[i].proc;
    if (m == WM_ACTIVATEAPP || m == WM_NCACTIVATE) w = TRUE;
    else if (m == WM_ACTIVATE) w = MAKEWPARAM(WA_ACTIVE, HIWORD(w));
    else if (m == WM_KILLFOCUS) return 0;
    /* The guest procedure is guest code: Windows' call into it faults on the
     * non-executable page and native32 runs the lifted body. */
    return proc ? CallWindowProcA((WNDPROC)(uintptr_t)proc, h, m, w, l) : DefWindowProcA(h, m, w, l);
}

static void shim_RegisterClassA(void) {
    WNDCLASSA c = *(const WNDCLASSA*)(uintptr_t)ARG(0);
    uint32_t guest = (uint32_t)(uintptr_t)c.lpfnWndProc;
    c.lpfnWndProc = hl_wndproc;
    ATOM a = RegisterClassA(&c);
    if (a && g_nclasses < MAX_CLASSES) {
        g_classes[g_nclasses].atom = a;
        g_classes[g_nclasses++].proc = guest;
    }
    g_eax = a;
    g_esp += 4 + 1 * 4;
}

static void shim_focus_query(void) {    /* GetActiveWindow, GetForegroundWindow, GetFocus */
    g_eax = (uint32_t)(uintptr_t)g_game_hwnd;
    g_esp += 4;
}

/* Dialogs the same way: a top-level one is created hidden (WS_VISIBLE
 * cleared in the template for the call), made invisible, then shown. */
static void shim_CreateDialogIndirectParamA(void) {
    uint8_t* t = (uint8_t*)(uintptr_t)ARG(1);
    int ex = *(uint16_t*)(t + 2) == 0xFFFF;             /* DLGTEMPLATEEX */
    uint32_t* style = (uint32_t*)(t + (ex ? 12 : 0));
    uint32_t saved = *style;
    int top = !(saved & WS_CHILD);
    HWND h;
    if (top) *style &= ~WS_VISIBLE;
    h = CreateDialogIndirectParamA((HINSTANCE)(uintptr_t)GUEST_MODULE(ARG(0)), (LPCDLGTEMPLATEA)t,
                                   (HWND)(uintptr_t)ARG(2), (DLGPROC)(uintptr_t)ARG(3), (LPARAM)ARG(4));
    *style = saved;
    if (h && top) {
        SetWindowLongA(h, GWL_EXSTYLE, GetWindowLongA(h, GWL_EXSTYLE) | HL_EXSTYLE);
        SetLayeredWindowAttributes(h, 0, 0, LWA_ALPHA);
        if (saved & WS_VISIBLE) ShowWindow(h, SW_SHOWNOACTIVATE);
    }
    if (h) input_dialog_created(h, g_last_dialog);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 5 * 4;
}

/* Scripted input answers the game's polls (input.c). */
static void shim_GetCursorPos(void) {
    POINT* p = (POINT*)(uintptr_t)ARG(0);
    g_eax = input_cursor(p) ? TRUE : GetCursorPos(p);
    g_esp += 4 + 1 * 4;
}

static void shim_GetKeyState(void) {
    g_eax = (uint16_t)input_key_state((int)ARG(0), GetKeyState((int)ARG(0)));
    g_esp += 4 + 1 * 4;
}

static void shim_GetAsyncKeyState(void) {
    g_eax = (uint16_t)input_key_state((int)ARG(0), GetAsyncKeyState((int)ARG(0)));
    g_esp += 4 + 1 * 4;
}

/* The game is single-instance by named mutex (an AppMutex at 0x006BBE56 and
 * friends), and a second copy exits at once. Headless runs are tests, and
 * tools/playtest.py runs several at a time, so each process gets its own
 * names (civ3 does the same). */
static void shim_CreateMutexA(void) {
    char name[MAX_PATH];
    const char* n = (const char*)(uintptr_t)ARG(2);
    if (n) _snprintf(name, sizeof name - 1, "%s.%lu", n, GetCurrentProcessId()), name[sizeof name - 1] = 0;
    g_eax = (uint32_t)(uintptr_t)CreateMutexA((LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(0), (BOOL)ARG(1),
                                             n ? name : NULL);
    g_esp += 4 + 3 * 4;
}

static void shim_OpenMutexA(void) {
    char name[MAX_PATH];
    const char* n = (const char*)(uintptr_t)ARG(2);
    _snprintf(name, sizeof name - 1, "%s.%lu", n ? n : "", GetCurrentProcessId()), name[sizeof name - 1] = 0;
    g_eax = (uint32_t)(uintptr_t)OpenMutexA(ARG(0), (BOOL)ARG(1), name);
    g_esp += 4 + 3 * 4;
}

static void shim_ShowWindow(void) {
    HWND h = (HWND)(uintptr_t)ARG(0);
    int cmd = (int)ARG(1);
    g_esp += 4 + 2 * 4;
    /* Shown without activation (the window is invisible anyway); then the
     * activation a fullscreen window gets on showing, delivered by hand. */
    g_eax = (uint32_t)ShowWindow(h, cmd == SW_HIDE ? SW_HIDE : SW_SHOWNOACTIVATE);
    if (h == g_game_hwnd && cmd != SW_HIDE) {
        SendMessageA(h, WM_ACTIVATEAPP, TRUE, 0);
        SendMessageA(h, WM_ACTIVATE, WA_ACTIVE, 0);
    }
}

/* ---- headless DirectDraw -------------------------------------------------
 * The game asks for an exclusive fullscreen 16-bit mode and draws by blitting
 * into the primary surface. Headless, the real DirectDraw object is kept and
 * three of its methods are patched in the (shared) IDirectDraw vtable:
 *
 *   SetCooperativeLevel  -> DDSCL_NORMAL, so nothing is exclusive
 *   SetDisplayMode       -> remembered, not applied
 *   GetDisplayMode       -> answers the remembered mode
 *   CreateSurface        -> the primary becomes an offscreen system-memory
 *                           surface of that mode; every surface without its own
 *                           pixel format gets the mode's, which a 32-bit
 *                           desktop would otherwise hand out
 *
 * The game never learns the difference: it blits into "the primary" and that
 * surface is what --record reads. */
static IDirectDrawSurface* g_primary;
/* The recorder reads the primary from its own thread, and a mode change
 * releases the primary on the game's. With only the pointer, the recorder
 * sometimes locked a freed surface (a fault in the recorder, on the lifted and
 * the original code alike, at the 800x600 -> 640x480 switch). So the host holds
 * a reference to the current primary, swaps it under this lock, and the
 * recorder reads it under the same lock. */
static CRITICAL_SECTION g_primary_lock;
static volatile LONG g_frames;       /* blits into the primary */

static const char* g_record;
static long g_record_frames;

typedef HRESULT (WINAPI *dd_coop_t)(IDirectDraw*, HWND, DWORD);
typedef HRESULT (WINAPI *dd_mode_t)(IDirectDraw*, DWORD, DWORD, DWORD);
typedef HRESULT (WINAPI *dd_getmode_t)(IDirectDraw*, LPDDSURFACEDESC);
typedef HRESULT (WINAPI *dd_surf_t)(IDirectDraw*, LPDDSURFACEDESC, LPDIRECTDRAWSURFACE*, IUnknown*);
typedef HRESULT (WINAPI *dds_bltfast_t)(IDirectDrawSurface*, DWORD, DWORD, IDirectDrawSurface*, LPRECT, DWORD);
static dds_bltfast_t g_real_bltfast;
typedef HRESULT (WINAPI *dds_blt_t)(IDirectDrawSurface*, LPRECT, IDirectDrawSurface*, LPRECT, DWORD, LPDDBLTFX);
static dd_coop_t g_real_coop;
static dd_mode_t g_real_mode;
static dd_getmode_t g_real_getmode;
static dd_surf_t g_real_surf;
static dds_blt_t g_real_blt;

static void mode_format(DDPIXELFORMAT* pf) {
    memset(pf, 0, sizeof *pf);
    pf->dwSize = sizeof *pf;
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = g_mode_bpp;
    if (g_mode_bpp == 16) { pf->dwRBitMask = 0xF800; pf->dwGBitMask = 0x07E0; pf->dwBBitMask = 0x001F; }
    else { pf->dwRBitMask = 0xFF0000; pf->dwGBitMask = 0x00FF00; pf->dwBBitMask = 0x0000FF; }
}

static HRESULT WINAPI hl_SetCooperativeLevel(IDirectDraw* dd, HWND h, DWORD flags) {
    /* Multithreaded: the presenter and the recorder lock the primary from
     * their own threads while the game draws on its. Without the flag
     * DirectDraw takes no lock of its own; Tiberian Sun, on the same host,
     * saw the display driver fault inside one of the game's Locks under load. */
    HRESULT hr = g_real_coop(dd, h, DDSCL_NORMAL | DDSCL_MULTITHREADED);
    fprintf(stderr, "[headless] SetCooperativeLevel(0x%lX) -> NORMAL|MULTITHREADED: 0x%08lX\n", flags, hr);
    return hr;
}

static HRESULT WINAPI hl_SetDisplayMode(IDirectDraw* dd, DWORD w, DWORD h, DWORD bpp) {
    (void)dd;
    g_mode_w = w, g_mode_h = h, g_mode_bpp = bpp, g_mode_set = 1;
    /* A real mode change resizes the fullscreen window to the new screen;
     * the game then sizes and centres things by it (the Bink intro). */
    if (g_game_hwnd) SetWindowPos(g_game_hwnd, NULL, 0, 0, (int)w, (int)h, SWP_NOZORDER | SWP_NOACTIVATE);
    input_mode_changed((int)w, (int)h);
    fprintf(stderr, "[headless] SetDisplayMode(%lux%lux%lu) -> kept, not applied\n", w, h, bpp);
    return DD_OK;
}

static HRESULT WINAPI hl_GetDisplayMode(IDirectDraw* dd, LPDDSURFACEDESC d) {
    HRESULT hr = g_real_getmode(dd, d);
    if (hr == DD_OK) {
        d->dwWidth = g_mode_w, d->dwHeight = g_mode_h;
        d->lPitch = g_mode_w * (g_mode_bpp / 8);
        mode_format(&d->ddpfPixelFormat);
    }
    return hr;
}

/* Every blit into the primary is a frame; the count is the boot's milestone. */
static HRESULT WINAPI hl_Blt(IDirectDrawSurface* dst, LPRECT r, IDirectDrawSurface* src, LPRECT sr,
                             DWORD flags, LPDDBLTFX fx) {
    if (dst == g_primary) {
        LONG n = InterlockedIncrement(&g_frames);
        if (n == 1 || n == 10 || n == 100 || n % 1000 == 0)
            fprintf(stderr, "[headless] frame %ld blitted to the primary\n", n);
    }
    return g_real_blt(dst, r, src, sr, flags, fx);
}

/* For the presenter: copy the primary when the game has put a new frame in
 * it, not every time it could. The menus blit into the primary; a battle
 * copies into its frame surface (hdvox.c). */
long host_frame_count(void) {
    extern volatile LONG ra2_frame_copies;
    return g_frames + ra2_frame_copies;
}

static void count_frame(void) {
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1 || n == 10 || n == 100 || n % 1000 == 0)
        fprintf(stderr, "[headless] frame %ld blitted to the primary\n", n);
}

static HRESULT WINAPI hl_BltFast(IDirectDrawSurface* dst, DWORD x, DWORD y, IDirectDrawSurface* src,
                                 LPRECT sr, DWORD flags) {
    if (dst == g_primary) count_frame();
    return g_real_bltfast(dst, x, y, src, sr, flags);
}

static HRESULT WINAPI hl_CreateSurface(IDirectDraw* dd, LPDDSURFACEDESC d, LPDIRECTDRAWSURFACE* out,
                                       IUnknown* outer) {
    DDSURFACEDESC c = *d;
    int primary = (d->dwFlags & DDSD_CAPS) && (d->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);
    if (primary) {
        c.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        c.dwWidth = g_mode_w, c.dwHeight = g_mode_h;
        c.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
        mode_format(&c.ddpfPixelFormat);
    } else if (!(d->dwFlags & DDSD_PIXELFORMAT)) {
        c.dwFlags |= DDSD_PIXELFORMAT;
        mode_format(&c.ddpfPixelFormat);
        /* Video memory will not take a format unlike the desktop's. */
        c.ddsCaps.dwCaps = (c.ddsCaps.dwCaps & ~(DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM))
                         | DDSCAPS_SYSTEMMEMORY;
        /* A surface with no type takes the display's format; one given a
         * format has to say it is offscreen, or DDERR_INVALIDPIXELFORMAT. */
        if (!(c.ddsCaps.dwCaps & (DDSCAPS_TEXTURE | DDSCAPS_OVERLAY | DDSCAPS_ZBUFFER)))
            c.ddsCaps.dwCaps |= DDSCAPS_OFFSCREENPLAIN;
        c.dwFlags |= DDSD_CAPS;
    }
    HRESULT hr = g_real_surf(dd, &c, out, outer);
    fprintf(stderr, "[headless] CreateSurface(flags 0x%lX caps 0x%lX %lux%lu)%s -> 0x%08lX %p\n",
            d->dwFlags, d->ddsCaps.dwCaps, c.dwWidth, c.dwHeight, primary ? " primary" : "", hr,
            hr == DD_OK ? (void*)*out : NULL);
    if (hr == DD_OK && primary) {
        IDirectDrawSurface* old;
        (*out)->lpVtbl->AddRef(*out);
        EnterCriticalSection(&g_primary_lock);
        old = g_primary;
        g_primary = *out;
        LeaveCriticalSection(&g_primary_lock);
        if (old) old->lpVtbl->Release(old);
        if (!g_real_blt) {
            void** vt = *(void***)g_primary;
            DWORD old;
            g_real_blt = (dds_blt_t)vt[5];                     /* IDirectDrawSurface::Blt */
            VirtualProtect(&vt[5], 4, PAGE_READWRITE, &old);
            vt[5] = (void*)hl_Blt;
            VirtualProtect(&vt[5], 4, old, &old);
            g_real_bltfast = (dds_bltfast_t)vt[7];             /* IDirectDrawSurface::BltFast */
            VirtualProtect(&vt[7], 4, PAGE_READWRITE, &old);
            vt[7] = (void*)hl_BltFast;
            VirtualProtect(&vt[7], 4, old, &old);
        }
    }
    return hr;
}

/* The game releases its DirectDraw object on the way out, and every surface
 * dies with it, the primary the host holds a reference to included. The
 * recorder then locked a freed surface (a fault at exit, on the shipping code
 * too). So when the object really goes, the primary goes with it. */
typedef ULONG (WINAPI *dd_release_t)(IDirectDraw*);
static dd_release_t g_real_ddrelease;

static ULONG WINAPI hl_DDRelease(IDirectDraw* dd) {
    EnterCriticalSection(&g_primary_lock);
    ULONG r = g_real_ddrelease(dd);
    if (r == 0) g_primary = NULL;
    LeaveCriticalSection(&g_primary_lock);
    return r;
}

static void patch(void** vt, int slot, void* fn, void** real) {
    DWORD old;
    *real = vt[slot];
    VirtualProtect(&vt[slot], 4, PAGE_READWRITE, &old);
    vt[slot] = fn;
    VirtualProtect(&vt[slot], 4, old, &old);
}

static void shim_DirectDrawCreate(void) {
    GUID* guid = (GUID*)(uintptr_t)ARG(0);
    IDirectDraw** out = (IDirectDraw**)(uintptr_t)ARG(1);
    HRESULT hr = DirectDrawCreate(guid, out, (IUnknown*)(uintptr_t)ARG(2));
    if (hr == DD_OK && !g_real_surf) {
        void** vt = *(void***)*out;
        patch(vt, 2, (void*)hl_DDRelease, (void**)&g_real_ddrelease);
        patch(vt, 6, (void*)hl_CreateSurface, (void**)&g_real_surf);
        patch(vt, 12, (void*)hl_GetDisplayMode, (void**)&g_real_getmode);
        patch(vt, 20, (void*)hl_SetCooperativeLevel, (void**)&g_real_coop);
        patch(vt, 21, (void*)hl_SetDisplayMode, (void**)&g_real_mode);
    }
    fprintf(stderr, "[headless] DirectDrawCreate -> 0x%08lX\n", hr);
    g_eax = (uint32_t)hr;
    g_esp += 4 + 3 * 4;
}

/* --record out.mp4: the primary, read at 30 fps from a host thread and piped
 * to ffmpeg as raw BGRX. Nothing is shown anywhere, so it works over RDP
 * (REPO_RULES 10/13). --frames N stops after N recorded frames and closes the
 * file properly; a process killed mid-recording leaves an mp4 with no index. */
static FILE* g_ffmpeg;
static DWORD g_rec_w, g_rec_h;
static long g_recorded;

/* The recorder writes on its thread; the watchdog and --frames close on
 * theirs. A close in the middle of a write freed the FILE under fwrite, and the
 * CRT ended the process with 0xC0000409. */
static CRITICAL_SECTION g_rec_lock;

static void record_close(void) {
    EnterCriticalSection(&g_rec_lock);
    if (g_ffmpeg) {
        _pclose(g_ffmpeg);
        g_ffmpeg = NULL;
        fprintf(stderr, "[record] %ld frames -> %s\n", g_recorded, g_record);
    }
    LeaveCriticalSection(&g_rec_lock);
}

/* The primary's pixels, copied out under its lock. The game waits on that
 * lock to draw, and converting (or composing HD voxels) inside it took 4.6 ms
 * a picture at 1352x845 on a Mac: the presenter kept the game at 40 frames a
 * second. The lock is now held for a memcpy, and the work runs on the copy.
 * One snapshot at a time (the presenter and the recorder share it). */
static uint8_t g_snap[4096 * 2160 * 4];
static CRITICAL_SECTION g_snap_lock;

/* With g_snap_lock held: the primary into g_snap, rows of *pitch bytes. */
static int snapshot(int* w, int* h, int* bpp, int* pitch) {
    DDSURFACEDESC d;
    EnterCriticalSection(&g_primary_lock);
    if (!g_primary) { LeaveCriticalSection(&g_primary_lock); return 0; }
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    if (g_primary->lpVtbl->Lock(g_primary, NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL) != DD_OK) {
        LeaveCriticalSection(&g_primary_lock);
        return 0;
    }
    *w = (int)d.dwWidth, *h = (int)d.dwHeight, *bpp = (int)d.ddpfPixelFormat.dwRGBBitCount;
    *pitch = *w * (*bpp / 8);
    int ok = *w <= 4096 && *h <= 2160 && (*bpp == 16 || *bpp == 32);
    for (int y = 0; ok && y < *h; y++)
        memcpy(g_snap + (size_t)y * *pitch, (const uint8_t*)d.lpSurface + (size_t)y * d.lPitch, (size_t)*pitch);
    g_primary->lpVtbl->Unlock(g_primary, NULL);
    LeaveCriticalSection(&g_primary_lock);
    return ok;
}

/* The game's picture: the primary, converted to 32-bit BGRX into `out`
 * (stride *w), at most maxw x maxh. 0 when there is no primary yet. Shared by
 * the recorder and the presenter (present.c). */
int host_frame(uint32_t* out, int maxw, int maxh, int* pw, int* ph) {
    int sw, sh, bpp, pitch;
    EnterCriticalSection(&g_snap_lock);
    if (!snapshot(&sw, &sh, &bpp, &pitch)) { LeaveCriticalSection(&g_snap_lock); return 0; }
    int w = sw < maxw ? sw : maxw, h = sh < maxh ? sh : maxh;
    for (int y = 0; y < h; y++) {
        const uint8_t* src = g_snap + (size_t)y * pitch;
        uint32_t* row = out + y * w;
        if (bpp == 16) {
            for (int x = 0; x < w; x++) {
                uint16_t p = ((const uint16_t*)src)[x];
                uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                row[x] = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            }
        } else memcpy(row, src, (size_t)w * 4);
    }
    LeaveCriticalSection(&g_snap_lock);
    *pw = w;
    *ph = h;
    return 1;
}

/* host_frame at twice the size, with the HD voxel layer (hdvox.c); 0 when HD
 * voxels are off, the frame is not 16-bit, or 2x would not fit. *pw, *ph get
 * the game's (1x) size. */
int host_frame_hd(uint32_t* out, int maxw, int maxh, int* pw, int* ph) {
    int w, h, bpp, pitch;
    if (!ra2_vox_hd_on) return 0;
    EnterCriticalSection(&g_snap_lock);
    if (!snapshot(&w, &h, &bpp, &pitch)) { LeaveCriticalSection(&g_snap_lock); return 0; }
    int ok = bpp == 16 && 2 * w <= maxw && 2 * h <= maxh;
    if (ok) hdvox_compose(g_snap, pitch, w, h, out);
    LeaveCriticalSection(&g_snap_lock);
    *pw = w;
    *ph = h;
    return ok;
}

/* --hd-voxels-dump DIR: every 10 s, the 2x frame as frame_NN.bmp (24-bit). */
static const char* g_hd_frames_dir;

static DWORD WINAPI hd_frame_dumper(LPVOID unused) {
    static uint32_t px[4096 * 2160];
    (void)unused;
    for (int n = 0; n < 60; ) {
        int w, h;
        Sleep(10000);
        if (!host_frame_hd(px, 4096, 2160, &w, &h)) continue;
        w *= 2, h *= 2;
        char path[MAX_PATH];
        _snprintf(path, sizeof path - 1, "%s/frame_%02d.bmp", g_hd_frames_dir, n++), path[sizeof path - 1] = 0;
        FILE* f = fopen(path, "wb");
        if (!f) continue;
        BITMAPINFOHEADER ih = { sizeof ih, w, -h, 1, 32, BI_RGB };
        uint32_t off = 14 + sizeof ih, size = off + (uint32_t)w * h * 4, zero = 0;
        fwrite("BM", 1, 2, f);
        fwrite(&size, 4, 1, f), fwrite(&zero, 4, 1, f), fwrite(&off, 4, 1, f);
        fwrite(&ih, sizeof ih, 1, f);
        fwrite(px, 4, (size_t)w * h, f);
        fclose(f);
    }
    return 0;
}

static DWORD WINAPI recorder(LPVOID unused) {
    static uint32_t frame[4096 * 2160], row[4096];
    DWORD next = GetTickCount();
    (void)unused;
    for (;;) {
        int w, h;
        next += 33;
        { LONG wait = (LONG)(next - GetTickCount()); if (wait > 0) Sleep(wait); }
        if (!host_frame(frame, 4096, 2160, &w, &h)) continue;
        EnterCriticalSection(&g_rec_lock);
        if (!g_ffmpeg && g_recorded) { LeaveCriticalSection(&g_rec_lock); return 0; }   /* closed */
        if (!g_ffmpeg) {
            char cmd[MAX_PATH * 2];
            /* At most 1280 wide: Red Alert 2 starts in the INI's mode, and x264
             * at 4K encoded 3 frames a second, so the recording, not the
             * game, looked stalled. */
            int k = (w + 1279) / 1280;
            g_rec_w = (DWORD)(w / k) & ~1u;
            g_rec_h = (DWORD)(h / k) & ~1u;
            _snprintf(cmd, sizeof cmd - 1, "ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgr0 "
                      "-s %lux%lu -r 30 -i - -c:v libx264 -pix_fmt yuv420p \"%s\"", g_rec_w, g_rec_h, g_record);
            g_ffmpeg = _popen(cmd, "wb");
            fprintf(stderr, "[record] %lux%lu -> %s\n", g_rec_w, g_rec_h, g_record);
        }
        if (g_recorded == 0 || g_recorded % 300 == 0) {
            uint32_t sum = 0;
            for (int k = 0; k < w * h; k += 16) sum = sum * 31 + frame[k];
            /* with the game's own frame count (its "Frame %d, BorrowedTime"
             * log line's): game frames per 10 s of recording is its frame rate */
            fprintf(stderr, "[record] frame %ld checksum %08X game frame %u\n", g_recorded, sum,
                    MEM32(RA2_GAME_FRAME_VA));
        }
        /* The recording keeps the size it started with (the menus' 800x600);
         * a later mode is scaled into it, nearest neighbour, with its aspect
         * kept: a 16:9 game is letterboxed, not squashed. Written at another
         * size, every frame after the mode change came out sheared. */
        {
            DWORD fw = g_rec_w, fh = (DWORD)((unsigned long long)g_rec_w * h / w);
            if (fh > g_rec_h) fh = g_rec_h, fw = (DWORD)((unsigned long long)g_rec_h * w / h);
            DWORD ox = (g_rec_w - fw) / 2, oy = (g_rec_h - fh) / 2;
            for (DWORD y = 0; y < g_rec_h && g_ffmpeg; y++) {
                if (y < oy || y >= oy + fh) {
                    memset(row, 0, g_rec_w * 4);
                } else {
                    const uint32_t* src = frame + ((y - oy) * (DWORD)h / fh) * (DWORD)w;
                    memset(row, 0, g_rec_w * 4);
                    for (DWORD x = 0; x < fw; x++) row[ox + x] = src[x * (DWORD)w / fw];
                }
                fwrite(row, 4, g_rec_w, g_ffmpeg);
            }
        }
        LeaveCriticalSection(&g_rec_lock);
        if (g_ffmpeg && ++g_recorded == g_record_frames) {
            record_close();
            fflush(stderr);
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
}

/* ---- the guest's identity ------------------------------------------------
 * The guest is gamemd.exe in the game folder, not this host. Its hInstance
 * (resources, window classes) comes from GetModuleHandleA(NULL), and it finds
 * its files relative to GetModuleFileNameA. */
static char g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 8];

static void shim_GetModuleHandleA(void) {
    g_eax = ARG(0) ? (uint32_t)(uintptr_t)GetModuleHandleA((LPCSTR)(uintptr_t)ARG(0))
                   : RA2_IMAGE_BASE;
    g_esp += 4 + 1 * 4;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == RA2_IMAGE_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            uint32_t k = n < size ? n : size - 1;
            memcpy(out, g_guest_exe, k);
            out[k] = 0;
            n = k;
        }
        g_eax = n;
    } else {
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size);
    }
    g_esp += 4 + 3 * 4;
}

static void shim_GetCommandLineA(void) {
    g_eax = (uint32_t)(uintptr_t)g_guest_cmdline;
    g_esp += 4;
}

/* ---- registration-free COM -----------------------------------------------
 * WinMain checks that Blowfish.dll (the Westwood Online cipher, a COM server
 * in the game folder) can be created, tries DllRegisterServer if not, and
 * exits with a fatal box if it still cannot. The Steam install script
 * registers it under HKCR, which needs admin; an unelevated self-registration
 * "succeeds" and writes nothing. So a class that is not registered is asked
 * of the game folder's own servers directly, the way a side-by-side manifest
 * would, and nothing is written to the registry. */
static const char* const g_com_servers[] = { "Blowfish.dll" };

typedef HRESULT (WINAPI *get_class_t)(REFCLSID, REFIID, void**);

static void shim_CoCreateInstance(void) {
    REFCLSID clsid = (REFCLSID)(uintptr_t)ARG(0);
    IUnknown* outer = (IUnknown*)(uintptr_t)ARG(1);
    REFIID iid = (REFIID)(uintptr_t)ARG(3);
    void** out = (void**)(uintptr_t)ARG(4);
    HRESULT hr = CoCreateInstance(clsid, outer, ARG(2), iid, out);
    for (int i = 0; hr == REGDB_E_CLASSNOTREG && i < (int)(sizeof g_com_servers / sizeof *g_com_servers); i++) {
        HMODULE m = LoadLibraryA(g_com_servers[i]);
        get_class_t get = m ? (get_class_t)GetProcAddress(m, "DllGetClassObject") : NULL;
        IClassFactory* f = NULL;
        if (get && get(clsid, &IID_IClassFactory, (void**)&f) == S_OK) {
            hr = f->lpVtbl->CreateInstance(f, outer, iid, out);
            f->lpVtbl->Release(f);
            fprintf(stderr, "[com] class %08lX not registered: served by %s -> 0x%08lX\n",
                    clsid->Data1, g_com_servers[i], hr);
        }
    }
    g_eax = (uint32_t)hr;
    g_esp += 4 + 5 * 4;
}

/* A NULL module means "the process's exe" to the resource and dialog calls,
 * and the process's exe is this host. The main menu is a dialog resource in
 * gamemd.exe: FindResourceA(NULL, 0xE2, RT_DIALOG) found nothing, and the
 * menu returned at once as if Exit had been picked (bringup.md, 9). */

static void shim_FindResourceA(void) {
    if (ARG(2) == (uint32_t)(uintptr_t)RT_DIALOG) g_last_dialog = (int)ARG(1);
    g_eax = (uint32_t)(uintptr_t)FindResourceA((HMODULE)(uintptr_t)GUEST_MODULE(ARG(0)),
                                               (LPCSTR)(uintptr_t)ARG(1), (LPCSTR)(uintptr_t)ARG(2));
    g_esp += 4 + 3 * 4;
}

static void shim_LoadResource(void) {
    g_eax = (uint32_t)(uintptr_t)LoadResource((HMODULE)(uintptr_t)GUEST_MODULE(ARG(0)),
                                              (HRSRC)(uintptr_t)ARG(1));
    g_esp += 4 + 2 * 4;
}

static void shim_CreateDialogParamA(void) {
    g_eax = (uint32_t)(uintptr_t)CreateDialogParamA((HINSTANCE)(uintptr_t)GUEST_MODULE(ARG(0)),
                                                    (LPCSTR)(uintptr_t)ARG(1), (HWND)(uintptr_t)ARG(2),
                                                    (DLGPROC)(uintptr_t)ARG(3), (LPARAM)ARG(4));
    g_esp += 4 + 5 * 4;
}

static void shim_DialogBoxParamA(void) {
    g_eax = (uint32_t)DialogBoxParamA((HINSTANCE)(uintptr_t)GUEST_MODULE(ARG(0)),
                                      (LPCSTR)(uintptr_t)ARG(1), (HWND)(uintptr_t)ARG(2),
                                      (DLGPROC)(uintptr_t)ARG(3), (LPARAM)ARG(4));
    g_esp += 4 + 5 * 4;
}

/* Multimedia timers. A guest timer callback runs on winmm's thread but only
 * once it gets the machine, which the game's thread gives up at its next
 * native call: timeKillEvent itself. So a callback that was already due ran
 * after the kill, after the game had deleted the critical section and freed
 * the sound buffer it uses: closing a movie with Escape faulted in DirectSound
 * and in the display driver under load (Tiberian Sun, on the same host). On Windows the
 * callback simply runs on to its end on its own thread. Here each timer
 * counts its callbacks in flight, and timeKillEvent returns once they are
 * done, with the machine given up while it waits. */
#define MAX_TIMERS 32
static struct { uint32_t proc, user; volatile UINT id; volatile LONG inflight; volatile DWORD in_thread; } g_timers[MAX_TIMERS];

static void CALLBACK timer_tramp(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR d1, DWORD_PTR d2) {
    int i = (int)user;
    InterlockedIncrement(&g_timers[i].inflight);
    if (g_timers[i].proc) {
        g_timers[i].in_thread = GetCurrentThreadId();
        ((LPTIMECALLBACK)(uintptr_t)g_timers[i].proc)(id, msg, g_timers[i].user, d1, d2);   /* the guest's */
        g_timers[i].in_thread = 0;
    }
    InterlockedDecrement(&g_timers[i].inflight);
}

static void shim_timeSetEvent(void) {
    UINT flags = ARG(4);
    int i = -1;
    if (!(flags & (TIME_CALLBACK_EVENT_SET | TIME_CALLBACK_EVENT_PULSE)))   /* a callback, not an event */
        for (int k = 0; k < MAX_TIMERS && i < 0; k++)
            if (!g_timers[k].proc && !g_timers[k].inflight) i = k;
    if (i < 0) {
        g_eax = timeSetEvent(ARG(0), ARG(1), (LPTIMECALLBACK)(uintptr_t)ARG(2), ARG(3), flags);
    } else {
        g_timers[i].proc = ARG(2), g_timers[i].user = ARG(3);
        g_eax = g_timers[i].id = timeSetEvent(ARG(0), ARG(1), timer_tramp, (DWORD_PTR)i, flags);
        if (!g_eax) g_timers[i].proc = 0;
    }
    g_esp += 4 + 5 * 4;
}

static void shim_timeKillEvent(void) {
    UINT id = ARG(0);
    int i = -1;
    for (int k = 0; k < MAX_TIMERS && i < 0; k++)
        if (g_timers[k].proc && g_timers[k].id == id) i = k;
    mach_leave();
    MMRESULT r = timeKillEvent(id);
    /* not when the callback kills its own timer: it is the one in flight */
    if (i >= 0 && g_timers[i].in_thread != GetCurrentThreadId())
        while (g_timers[i].inflight) Sleep(1);
    mach_enter();
    if (i >= 0) g_timers[i].proc = 0, g_timers[i].id = 0;
    g_eax = r;
    g_esp += 4 + 1 * 4;
}

#define TIMER_SHIMS \
    { "timeSetEvent", shim_timeSetEvent }, \
    { "timeKillEvent", shim_timeKillEvent }

#define GUEST_SHIMS \
    { "FindResourceA", shim_FindResourceA }, \
    { "LoadResource", shim_LoadResource }, \
    { "CreateDialogParamA", shim_CreateDialogParamA }, \
    { "DialogBoxParamA", shim_DialogBoxParamA }, \
    { "CoCreateInstance", shim_CoCreateInstance }, \
    { "GetModuleHandleA", shim_GetModuleHandleA }, \
    { "GetModuleFileNameA", shim_GetModuleFileNameA }, \
    { "GetCommandLineA", shim_GetCommandLineA }

static native32_shim_t g_shims[] = { GUEST_SHIMS, TIMER_SHIMS };

static native32_shim_t g_headless_shims[] = {
    GUEST_SHIMS,
    TIMER_SHIMS,
    { "MessageBoxA", shim_MessageBoxA },
    { "CreateWindowExA", shim_CreateWindowExA },
    { "ShowWindow", shim_ShowWindow },
    { "RegisterClassA", shim_RegisterClassA },
    { "CreateDialogIndirectParamA", shim_CreateDialogIndirectParamA },
    { "GetCursorPos", shim_GetCursorPos },
    { "CreateMutexA", shim_CreateMutexA },
    { "OpenMutexA", shim_OpenMutexA },
    { "GetKeyState", shim_GetKeyState },
    { "GetAsyncKeyState", shim_GetAsyncKeyState },
    { "GetActiveWindow", shim_focus_query },
    { "GetForegroundWindow", shim_focus_query },
    { "GetFocus", shim_focus_query },
    { "ClientToScreen", shim_ClientToScreen },
    { "ScreenToClient", shim_ScreenToClient },
    { "GetWindowRect", shim_GetWindowRect },
    { "SendMessageA", shim_SendMessageA },
    { "WindowFromPoint", shim_WindowFromPoint },
    { "MoveWindow", shim_MoveWindow },
    { "SetWindowPos", shim_SetWindowPos },
    { "SetCursorPos", shim_SetCursorPos },
    { "SetCursor", shim_SetCursor },
    { "SetForegroundWindow", shim_SetForegroundWindow },
    { "BringWindowToTop", shim_BringWindowToTop },
    { "SetActiveWindow", shim_SetActiveWindow },
    { "GetSystemMetrics", shim_GetSystemMetrics },
    { "DirectDrawCreate", shim_DirectDrawCreate },
};

/* ---- hooks: lifted functions given a host body (run_lift.py HOOKS) ------- */

/* The game's debug printf, compiled out of the retail build. --debuglog
 * prints it: printf-style, the arguments read straight off the guest stack
 * (a cdecl va_list on x86 is a pointer to the first variadic slot). */
static int g_debuglog;

/* --mute: this process's audio session at zero (Vista and later: waveOut's
 * volume is the session's, and DirectSound and Bink play in the same
 * session). Held every second, since the game opens its devices later. For
 * test runs on a machine someone is using. */
static DWORD WINAPI mute_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        waveOutSetVolume(NULL, 0);
        Sleep(1000);
    }
}

void ra2_hook_debuglog(void) {
    if (g_debuglog || input_wants_log()) {
        char buf[1024];
        const char* fmt = (const char*)(uintptr_t)MEM32(g_esp + 4);
        size_t n;
        _vsnprintf(buf, sizeof buf - 1, fmt, (va_list)(uintptr_t)(g_esp + 8));
        buf[sizeof buf - 1] = 0;
        n = strlen(buf);
        input_log_line(buf);
        if (g_debuglog) fprintf(stderr, "[game] %s%s", buf, n && buf[n - 1] == '\n' ? "" : "\n");
    }
    g_esp += 4;                        /* ret */
}

/* ---- diagnostics ---------------------------------------------------------
 * --probe VA (repeatable): report indirect calls to VA -- a virtual method or
 * callback -- with `this` and the first arguments. RECOMP_ICALL asks this
 * hook before the dispatch table, so it costs nothing when unset. */
#define MAX_PROBES 8
static uint32_t g_probe[MAX_PROBES];
static int g_nprobe;
static volatile LONG g_probe_hits[MAX_PROBES];

recomp_func_t recomp_lookup_manual(uint32_t va) {
    for (int i = 0; i < g_nprobe; i++)
        if (g_probe[i] == va && InterlockedIncrement(&g_probe_hits[i]) <= 5)
            fprintf(stderr, "[probe] sub_%08X from sub_%08X  ecx=%08X  args %08X %08X %08X\n",
                    va, g_cur_func, g_ecx, MEM32(g_esp), MEM32(g_esp + 4), MEM32(g_esp + 8));
    return NULL;
}

static void probe_report(void) {
    for (int i = 0; i < g_nprobe; i++)
        fprintf(stderr, "[probe] sub_%08X: %ld calls\n", g_probe[i], g_probe_hits[i]);
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr,
        "\n[not-lifted] sub_%08X  (called from 0x%08X)\n"
        "  Widen the closure:  py -3 run_lift.py --roots 0x%08X  (or --max N, or --all)\n",
        va, g_cur_func, va);
    recomp_dump_trace("not-lifted");
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

/* Added after native32's own handler, so callbacks are resolved first and
 * only real faults get here. The report goes out through WriteFile from a
 * static buffer, not stdio: a fault while another thread holds the CRT's
 * stderr lock, or with no stack left, otherwise ends with no report at all
 * (The Movies, docs/bringup.md). */
static char g_crash_buf[4096];
static int g_crash_len;
static void crash_emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(g_crash_buf + g_crash_len, sizeof g_crash_buf - 1 - g_crash_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_len += n;
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    crash_emit("\n=== fault 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        crash_emit("  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                   native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    crash_emit("  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    crash_emit("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    crash_emit("last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = native32_name(g_icall_trace[k]);
        crash_emit("  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
    /* After the report, which must not wait on it: --calltrace buffers 4 MB,
     * and without this the entries nearest the fault were the ones lost. */
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* RA2_PROFILE=1: which lifted function is running, sampled every millisecond
 * (the last one entered), the most frequent printed when the watchdog ends
 * the run. How the voxel renderer was found (docs/voxels.md). */
#define PROF_SLOTS 65536
static struct { uint32_t va, n; } g_prof[PROF_SLOTS];
static DWORD WINAPI profiler(LPVOID unused) {
    (void)unused;
    timeBeginPeriod(1);
    for (;;) {
        uint32_t va = g_cur_func;
        for (uint32_t h = (va * 2654435761u) >> 16, k = 0; va && k < PROF_SLOTS; k++, h = (h + 1) & (PROF_SLOTS - 1))
            if (g_prof[h].va == va || !g_prof[h].va) { g_prof[h].va = va; g_prof[h].n++; break; }
        Sleep(1);
    }
}
static int prof_cmp(const void* a, const void* b) {
    return (int)((const uint32_t*)b)[1] - (int)((const uint32_t*)a)[1];
}
static void profile_report(void) {
    static uint32_t top[PROF_SLOTS][2];
    int n = 0;
    uint64_t total = 0;
    for (int i = 0; i < PROF_SLOTS; i++)
        if (g_prof[i].va) top[n][0] = g_prof[i].va, top[n][1] = g_prof[i].n, total += g_prof[i].n, n++;
    if (!n) return;
    qsort(top, n, sizeof top[0], prof_cmp);
    for (int i = 0; i < n && i < 40; i++)
        fprintf(stderr, "[profile] sub_%08X %6u  %4.1f%%\n", top[i][0], top[i][1], 100.0 * top[i][1] / total);
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, g_frames);
    native32_dump_icalls(8);
    probe_report();
    profile_report();
    record_close();
    recomp_trace_flush();
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* exe = "game\\" RA2_EXE_NAME;
    const char* game = "game";
    char exe_full[MAX_PATH], game_full[MAX_PATH];
    int run = 0;
    /* stderr unbuffered: the log is the evidence, and the game leaves through
     * ExitProcess, which flushes nothing with the C runtime linked in (/MT):
     * a run that exited cleanly lost its last 4 KB, menus and all. */
    setvbuf(stderr, NULL, _IONBF, 0);
    /* --args FILE: more arguments, whitespace-separated, # to the end of a
     * line is a comment. A script that is a list of presses, for a caller
     * that can pass only one word (a launcher's setting, a scheduled task). */
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--args")) continue;
        static char text[16384];
        static char* av[512];
        FILE* f = fopen(argv[i + 1], "rb");
        size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
        int ac = 0;
        if (f) fclose(f);
        if (!f) { fprintf(stderr, "cannot read --args %s\n", argv[i + 1]); return 1; }
        text[n] = 0;
        for (int k = 0; k < argc && ac < 500; k++)
            if (k != i && k != i + 1) av[ac++] = argv[k];
        for (char* p = text; *p && ac < 511;) {
            while (*p && strchr(" \t\r\n", *p)) p++;
            if (*p == '#') { while (*p && *p != '\n') p++; continue; }
            if (!*p) break;
            av[ac++] = p;
            while (*p && !strchr(" \t\r\n", *p)) p++;
            if (*p) *p++ = 0;
        }
        argc = ac, argv = av;
        break;
    }
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (!n) n = input_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--classic")) g_classic = 1;
        else if (!strcmp(argv[i], "--fullscreen")) g_fullscreen = 1;
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc && present_mode_from_name(argv[i + 1]) >= 0)
            g_scale_mode = present_mode_from_name(argv[++i]);
        else if (!strcmp(argv[i], "--debuglog")) g_debuglog = 1;
        else if (!strcmp(argv[i], "--hd-voxels")) hdvox_configure(1, NULL);
        else if (!strcmp(argv[i], "--mute")) CloseHandle(CreateThread(NULL, 0, mute_thread, NULL, 0, NULL));
        else if (!strcmp(argv[i], "--hd-voxels-dump") && i + 1 < argc) {
            static char dir[MAX_PATH];
            GetFullPathNameA(argv[++i], MAX_PATH, dir, NULL);   /* the run chdirs into game/ */
            hdvox_configure(1, dir);
            g_hd_frames_dir = dir;
        }
        else if (!strcmp(argv[i], "--original")) g_original = 1;
        else if (!strcmp(argv[i], "--probe") && i + 1 < argc && g_nprobe < MAX_PROBES)
            g_probe[g_nprobe++] = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) g_record = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) g_record_frames = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: ra2 [--run] [--headless | --classic | [--fullscreen] [--scale sharp|smooth|crt|nearest|integer]] [--record out.mp4] [--frames N] [--exe game\\" RA2_EXE_NAME "] [--game game]\n"
                   "           [--press DLG:CTRL@s] [--select DLG:CTRL=N@s] [--waitlog TEXT@s] [--move|--click x,y@s] [--key [c][s][a]+vk@s] [--wait VA@s]\n"
                   "           [--watchdog S] [--probe VA] [--debuglog] [--original] [--native-trace] [--callbacks]\n"
                   "           [--hd-voxels] [--hd-voxels-dump DIR] [--mute] [--args FILE]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    if (g_record && g_classic) { fprintf(stderr, "--record needs the virtual display (not --classic)\n"); return 1; }
    GetFullPathNameA(exe, MAX_PATH, exe_full, NULL);
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    if (g_record) {                    /* the run chdirs into game\ */
        static char rec_full[MAX_PATH];
        GetFullPathNameA(g_record, MAX_PATH, rec_full, NULL);
        g_record = rec_full;
    }
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%s\\" RA2_EXE_NAME, game_full);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);

    /* binkw32.dll ships in the game folder, so imports bind from there. But the
     * folder also carries DDrawCompat as ddraw.dll, and that is a fullscreen
     * shim of its own: load the system's DirectDraw first, by full path, so the
     * game's DDRAW.dll import binds to it. */
    {
        char sys[MAX_PATH];
        GetSystemDirectoryA(sys, MAX_PATH);
        strcat_s(sys, sizeof sys, "\\ddraw.dll");
        if (!LoadLibraryA(sys)) fprintf(stderr, "warning: cannot load %s\n", sys);
        SetDllDirectoryA(game_full);
    }

    InitializeCriticalSection(&g_primary_lock);
    InitializeCriticalSection(&g_snap_lock);
    InitializeCriticalSection(&g_rec_lock);
    /* Headless and the presenter share the virtual display. The presenter
     * keeps the game's message boxes real (a player answers them) and its
     * single-instance mutexes as they are. */
    native32_shim_t* shims = g_classic ? g_shims : g_headless_shims;
    int nshims = g_classic ? (int)(sizeof g_shims / sizeof g_shims[0])
                           : (int)(sizeof g_headless_shims / sizeof g_headless_shims[0]);
    if (!g_classic && !g_headless) {
        static native32_shim_t live[sizeof g_headless_shims / sizeof g_headless_shims[0]];
        int n = 0;
        for (int k = 0; k < nshims; k++)
            if (strcmp(shims[k].name, "MessageBoxA") && strcmp(shims[k].name, "CreateMutexA") &&
                strcmp(shims[k].name, "OpenMutexA"))
                live[n++] = shims[k];
        shims = live, nshims = n;
    }
    if (g_original) {
        /* The shipping machine code under the same host, shims and input
         * (oracle.c): the reference a lifted run is compared against. */
        static const oracle_hook_t hooks[] = { { RA2_HOOK_DEBUGLOG_VA, ra2_hook_debuglog } };
        if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
        if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        if (getenv("RA2_PROFILE")) CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL));
        if (g_record) CloseHandle(CreateThread(NULL, 0, recorder, NULL, 0, NULL));
        input_start();
        if (!g_classic && !g_headless) present_start(g_scale_mode, g_fullscreen);
        if (g_hd_frames_dir) CloseHandle(CreateThread(NULL, 0, hd_frame_dumper, NULL, 0, NULL));
        return oracle_run(exe_full, RA2_IMAGE_BASE, shims, nshims, hooks, 1);
    }

    native32_init();
    AddVectoredExceptionHandler(0, crash);
    printf(RA2_TITLE " recomp host\n  lifted functions in dispatch: %u\n",
           recomp_dispatch_count);

    uint32_t span = native32_map(exe_full, RA2_IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, RA2_IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", exe, RA2_IMAGE_BASE, RA2_IMAGE_BASE + span);
    if (native32_bind(RA2_IMAGE_BASE, shims, nshims)) return 1;
    printf("  guest exe %s\n", g_guest_exe);

    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", ra2_entry_va);
        return 0;
    }
    /* The game opens its MIX files relative to its working directory. */
    if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    if (getenv("RA2_PROFILE")) CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL));
    if (g_record) CloseHandle(CreateThread(NULL, 0, recorder, NULL, 0, NULL));
    /* A script drives the virtual display, headless or in the presenter (the
     * lab's LAN games: a window to snap, a script to play); the script's
     * keys and buttons win over the live ones. Not --classic: that is a real
     * screen and a real mouse. */
    if (input_scripted() && g_classic) { fprintf(stderr, "a scripted run cannot use --classic\n"); return 1; }
    input_start();
    if (!g_classic && !g_headless) present_start(g_scale_mode, g_fullscreen);
    if (g_hd_frames_dir) CloseHandle(CreateThread(NULL, 0, hd_frame_dumper, NULL, 0, NULL));
    printf("  entering 0x%08X\n\n", ra2_entry_va);
    fflush(stdout);
    native32_call_guest(ra2_entry_va, 0, NULL);
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
