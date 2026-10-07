/*
 * Scripted input for headless runs (docs/testing.md).
 *
 *   --press DLG:CTRL@s   press control CTRL of menu dialog DLG, once DLG is open
 *   --select DLG:CTRL=N@s  pick item N of a list or combo box in DLG
 *   --waitlog TEXT@s     hold until the game's debug log prints a line with TEXT
 *   --move x,y@s  --click [c][s][a]+x,y@s  --key [c][s][a]+vk@s  --wait VA@s
 *   (c, s, a: held down with it: Ctrl, Shift, Alt; Ctrl+click is force-fire)
 *   --drag x1,y1,x2,y2@s   a band selection, button down at one corner, up at the other
 *
 * Civilization III's grammar and timing (civ3 src/runtime/input.c): s is
 * seconds after the main menu opened, events run in time order on one thread,
 * and a wait moves every later event back by however long it took. Nothing
 * touches the real cursor or keyboard: input is posted window messages, and
 * GetCursorPos/GetKeyState/GetAsyncKeyState answer from the script.
 *
 * What RA2 adds is --press. Its menus are Win32 dialogs from the exe's
 * resources (tools/dialogs.py maps all 98), so a script names a button by
 * dialog and control ID instead of a pixel, and waits for the dialog to be
 * open rather than for a time. The host learns each dialog's resource ID from
 * the FindResourceA(RT_DIALOG) the game makes just before creating it.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input.h"

typedef struct { char kind; int x, y, mods; double t; char text[64]; } ev_t;
#define MAX_EV 256
static ev_t g_ev[MAX_EV];
static int g_nev;
static volatile LONG g_cx = -1, g_cy = -1;

int input_arg(int argc, char** argv, int i) {
    if (i + 1 >= argc || g_nev >= MAX_EV) return 0;
    ev_t e = { 0 };
    const char* a = argv[i + 1];
    if (!strcmp(argv[i], "--move") || !strcmp(argv[i], "--click")) {
        const char* plus = strchr(a, '+');
        if (plus) {
            for (const char* m = a; m < plus; m++)
                e.mods |= *m == 'c' ? 1 : *m == 's' ? 2 : *m == 'a' ? 4 : 0;
            a = plus + 1;
        }
        if (sscanf(a, "%d,%d@%lf", &e.x, &e.y, &e.t) != 3) return 0;
        e.kind = argv[i][2];                    /* 'm' or 'c' */
    } else if (!strcmp(argv[i], "--drag")) {
        int x2, y2;
        if (sscanf(a, "%d,%d,%d,%d@%lf", &e.x, &e.y, &x2, &y2, &e.t) != 5) return 0;
        e.mods = x2 << 16 | (y2 & 0xFFFF);      /* the far corner */
        e.kind = 'd';
    } else if (!strcmp(argv[i], "--press")) {
        if (sscanf(a, "%i:%i@%lf", &e.x, &e.y, &e.t) != 3) return 0;
        e.kind = 'p';
    } else if (!strcmp(argv[i], "--select")) {
        int n;
        if (sscanf(a, "%i:%i=%d@%lf", &e.x, &e.y, &n, &e.t) != 4) return 0;
        e.y |= n << 16;                 /* item index in the high half */
        e.kind = 's';
    } else if (!strcmp(argv[i], "--waitlog")) {
        /* Burnout 3's state signal (xboxrecomp's [PATH] lines): what the game
         * says it is doing, not how long it has taken. "Capture_Mouse" is
         * printed when a game starts. */
        const char* at = strrchr(a, '@');
        if (!at || at - a >= (int)sizeof e.text || sscanf(at + 1, "%lf", &e.t) != 1) return 0;
        memcpy(e.text, a, at - a);
        e.kind = 'l';
    } else if (!strcmp(argv[i], "--wait")) {
        /* --wait VA@s: hold the script until the dword at VA is nonzero. */
        if (sscanf(a, "%i@%lf", &e.x, &e.t) != 2) return 0;
        e.kind = 'w';
    } else if (!strcmp(argv[i], "--key")) {
        const char* plus = strchr(a, '+');
        if (plus) {
            for (const char* m = a; m < plus; m++)
                e.y |= *m == 'c' ? 1 : *m == 's' ? 2 : *m == 'a' ? 4 : 0;
            a = plus + 1;
        }
        if (sscanf(a, "%i@%lf", &e.x, &e.t) != 2) return 0;
        e.kind = 'k';
    } else return 0;
    g_ev[g_nev++] = e;
    return 2;
}

/* ---- open dialogs ---------------------------------------------------------
 * A small table of (window, resource ID). The host reports each creation; a
 * poller notices when one goes away. Both print [dialog] lines, which are the
 * milestones tools/playtest.py and tools/conformance.py read. */
#define MAX_DLG 32
static struct { HWND h; int id; } g_dlg[MAX_DLG];
static CRITICAL_SECTION g_dlg_lock;
static volatile LONG g_menu_open;      /* the main menu (0xE2) has opened once */
static DWORD g_t0;

void input_dialog_created(HWND h, int id) {
    EnterCriticalSection(&g_dlg_lock);
    for (int i = 0; i < MAX_DLG; i++)
        if (!g_dlg[i].h || !IsWindow(g_dlg[i].h)) { g_dlg[i].h = h; g_dlg[i].id = id; break; }
    LeaveCriticalSection(&g_dlg_lock);
    InterlockedIncrement(&g_dialogs_opened);
    fprintf(stderr, "[dialog] open 0x%X\n", id);
    if (id == 0xE2 && !InterlockedExchange(&g_menu_open, 1)) g_t0 = GetTickCount();
}

static HWND find_dialog(int id) {
    HWND h = NULL;
    EnterCriticalSection(&g_dlg_lock);
    for (int i = 0; i < MAX_DLG && !h; i++)
        if (g_dlg[i].id == id && g_dlg[i].h && IsWindow(g_dlg[i].h)) h = g_dlg[i].h;
    LeaveCriticalSection(&g_dlg_lock);
    return h;
}

static DWORD WINAPI dialog_poller(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(100);
        EnterCriticalSection(&g_dlg_lock);
        for (int i = 0; i < MAX_DLG; i++)
            if (g_dlg[i].h && !IsWindow(g_dlg[i].h)) {
                fprintf(stderr, "[dialog] closed 0x%X\n", g_dlg[i].id);
                g_dlg[i].h = NULL;
            }
        LeaveCriticalSection(&g_dlg_lock);
    }
}

/* ---- the game's debug log (host.c, ra2_hook_004068E0) ------------------- */
static const char* volatile g_want_log;     /* what --waitlog waits for */
static volatile LONG g_saw_log;

/* The last lines, with when they came, so a wait also sees a line printed
 * since the previous step began: an order's own echo ("Adding event DEPLOY")
 * lands while the key that gave it is still held, before the wait starts. */
#define RING 64
static struct { DWORD t; char s[160]; } g_ring[RING];
static volatile LONG g_ring_n;
static volatile DWORD g_step_t0;               /* when the previous step began */

int input_wants_log(void) { return g_want_log != NULL || g_nev > 0; }

void input_log_line(const char* line) {
    const char* w = g_want_log;
    LONG k = InterlockedIncrement(&g_ring_n) - 1;
    g_ring[k % RING].t = GetTickCount();
    strncpy(g_ring[k % RING].s, line, sizeof g_ring[0].s - 1);
    g_ring[k % RING].s[sizeof g_ring[0].s - 1] = 0;
    if (w && strstr(line, w)) InterlockedExchange(&g_saw_log, 1);
}

static int seen_since(const char* text, DWORD since) {
    LONG n = g_ring_n;
    for (LONG k = n - 1; k >= 0 && k >= n - RING; k--)
        if ((LONG)(g_ring[k % RING].t - since) >= 0 && strstr(g_ring[k % RING].s, text)) return 1;
    return 0;
}

/* ---- what the game reads --------------------------------------------------- */
/* A display mode change keeps the cursor on the screen, as Windows does. The
 * menus are 800x600 and a game 640x480: a cursor left on a menu button at
 * x=720 was past the right edge in game, and the game's edge scrolling ran the
 * view to the black margin of the map and kept it there (docs/testing.md). */
void input_mode_changed(int w, int h) {
    if (g_cx >= w) InterlockedExchange(&g_cx, w - 1);
    if (g_cy >= h) InterlockedExchange(&g_cy, h - 1);
}

BOOL input_cursor(POINT* p) {
    if (g_cx < 0) return FALSE;
    p->x = g_cx;            /* client = screen for the game (host.c, ClientToScreen) */
    p->y = g_cy;
    return TRUE;
}

/* Live input: the presenter (present.c) feeds the cursor, mapped into game
 * coordinates, and keys are posted to the game's windows. Posted keys do not
 * move the game thread's key state, so GetKeyState answers from the physical
 * keyboard, which is the presenter's (same process, same desktop). */
static int g_live;
static volatile LONG g_live_buttons;      /* MK_LBUTTON | MK_RBUTTON | MK_MBUTTON */
void input_live(int on) { g_live = on; }
/* The mouse buttons as the presenter's window saw them: the game's controls
 * read VK_LBUTTON as well as the messages, and a click the presenter got
 * (from a mouse, a pen, a touch screen or a test driver) has to agree. */
void input_live_buttons(int mk) { InterlockedExchange(&g_live_buttons, mk); }
void input_live_cursor(int x, int y) {
    InterlockedExchange(&g_cx, x);
    InterlockedExchange(&g_cy, y);
}

/* Mouse messages posted to game windows, until the game's thread takes them
 * (host.c's Wine hook asks): everything else it gets is the real mouse. */
#define NPOSTED 64
static struct { HWND h; UINT m; LPARAM l; } g_posted[NPOSTED];
static int g_nposted;
static CRITICAL_SECTION g_posted_lock;
static INIT_ONCE g_posted_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK posted_init(PINIT_ONCE o, PVOID p, PVOID* c) {
    (void)o, (void)p, (void)c;
    InitializeCriticalSection(&g_posted_lock);
    return TRUE;
}

void input_post_mouse(HWND h, UINT m, WPARAM w, LPARAM l) {
    InitOnceExecuteOnce(&g_posted_once, posted_init, NULL, NULL);
    EnterCriticalSection(&g_posted_lock);
    if (g_nposted == NPOSTED) memmove(g_posted, g_posted + 1, sizeof g_posted - sizeof g_posted[0]), g_nposted--;
    g_posted[g_nposted].h = h, g_posted[g_nposted].m = m, g_posted[g_nposted++].l = l;
    LeaveCriticalSection(&g_posted_lock);
    PostMessageA(h, m, w, l);
}

/* The same question without taking the note: a PeekMessage that leaves the
 * message in the queue. */
int input_posted_mouse_peek(HWND h, UINT m, LPARAM l) {
    int found = 0;
    InitOnceExecuteOnce(&g_posted_once, posted_init, NULL, NULL);
    EnterCriticalSection(&g_posted_lock);
    for (int i = 0; i < g_nposted && !found; i++)
        found = g_posted[i].h == h && g_posted[i].m == m && g_posted[i].l == l;
    LeaveCriticalSection(&g_posted_lock);
    return found;
}

int input_posted_mouse(HWND h, UINT m, LPARAM l) {
    int found = 0;
    InitOnceExecuteOnce(&g_posted_once, posted_init, NULL, NULL);
    EnterCriticalSection(&g_posted_lock);
    for (int i = 0; i < g_nposted; i++)
        if (g_posted[i].h == h && g_posted[i].m == m && g_posted[i].l == l) {
            memmove(g_posted + i, g_posted + i + 1, (g_nposted - i - 1) * sizeof g_posted[0]);
            g_nposted--, found = 1;
            break;
        }
    LeaveCriticalSection(&g_posted_lock);
    return found;
}

static volatile LONG g_mods, g_lb_reads;
SHORT input_key_state(int vk, SHORT real) {
    if (!g_nev && g_live) {
        int mk = vk == VK_LBUTTON ? MK_LBUTTON : vk == VK_RBUTTON ? MK_RBUTTON : vk == VK_MBUTTON ? MK_MBUTTON : 0;
        if (mk) return (g_live_buttons & mk) ? (SHORT)0x8000 : 0;
        return (SHORT)((GetAsyncKeyState(vk) & 0x8000) | (real & 1));
    }
    if (!g_nev) return real;
    if (vk == VK_LBUTTON) InterlockedIncrement(&g_lb_reads);
    int bit = vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ? 1
            : vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ? 2
            : vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ? 4
            : vk == VK_LBUTTON ? 8 : 0;
    /* A scripted run answers every key from the script: the console's (or the
     * RDP phone's) real keyboard never leaks into a test. */
    return (bit && (g_mods & bit)) ? (SHORT)0x8000 : 0;
}

/* Wait until the game has read the left button twice more, up to 1 s. */
static void lb_seen(void) {
    LONG r0 = g_lb_reads;
    for (int i = 0; i < 40 && g_lb_reads - r0 < 2; i++) Sleep(25);
}

/* Held until the game has seen it down, then up (civ3: a fixed hold was
 * missed when several runs at once stretched a frame past it). */
static void click(HWND h, LPARAM lp) {
    InterlockedOr(&g_mods, 8);
    input_post_mouse(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    Sleep(250);
    lb_seen();
    input_post_mouse(h, WM_LBUTTONUP, 0, lp);
    InterlockedAnd(&g_mods, ~8);
    lb_seen();
}

/* Press a dialog control: point the cursor at it and click it, the way a
 * player does. If the dialog is still open and nothing new opened 3 s later,
 * send the BN_CLICKED a button sends its parent. Returns the method used, or
 * NULL if the dialog never opened. */
/* press()'s view of the dialog's thread (in-process thread hooks): the
 * button-up it takes off its queue, and the BN_CLICKED the button sends. */
static HWND g_watch_btn, g_watch_dlg;
static int g_watch_ctrl;
static volatile LONG g_watch_up, g_watch_cmd;

static LRESULT CALLBACK watch_getmsg(int code, WPARAM wp, LPARAM lp) {
    const MSG* m = (const MSG*)lp;
    if (code >= 0 && wp == PM_REMOVE && m->message == WM_LBUTTONUP && m->hwnd == g_watch_btn)
        InterlockedIncrement(&g_watch_up);
    return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK watch_callwnd(int code, WPARAM wp, LPARAM lp) {
    const CWPSTRUCT* m = (const CWPSTRUCT*)lp;
    /* to whichever window is the button's parent: a control inside a panel
     * of the dialog tells the panel, not the dialog */
    if (code >= 0 && m->message == WM_COMMAND && (HWND)m->lParam == g_watch_btn &&
        LOWORD(m->wParam) == g_watch_ctrl && HIWORD(m->wParam) == BN_CLICKED)
        InterlockedIncrement(&g_watch_cmd);
    return CallNextHookEx(NULL, code, wp, lp);
}

static const char* press(int dlg, int ctrl, LONG* shift) {
    DWORD w0 = GetTickCount();
    HWND d;
    while (!(d = find_dialog(dlg)) && GetTickCount() - w0 < 600000) Sleep(100);
    *shift += (LONG)(GetTickCount() - w0);
    if (!d) return NULL;
    Sleep(1500);                        /* the menus slide their buttons in */
    HWND c = GetDlgItem(d, ctrl);
    if (!c) return "no such control";
    RECT r;
    GetWindowRect(c, &r);
    POINT mid = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 }, local = mid;
    MapWindowPoints(NULL, g_input_hwnd, &mid, 1);          /* game client */
    /* A static control answers WM_NCHITTEST with HTTRANSPARENT, so a real
     * click on it lands on the dialog underneath: the campaign screen's
     * emblems are statics, and the dialog procedure reads the click there. */
    char cls[16] = "";
    GetClassNameA(c, cls, sizeof cls);
    if (!_stricmp(cls, "Static")) c = d;
    ScreenToClient(c, &local);
    InterlockedExchange(&g_cx, mid.x);
    InterlockedExchange(&g_cy, mid.y);
    input_post_mouse(c, WM_MOUSEMOVE, 0, MAKELPARAM(local.x, local.y));
    Sleep(200);
    LONG opens = g_dialogs_opened;
    /* Watch the dialog's thread: when it takes the click's button-up off its
     * queue, and whether the button then sends its BN_CLICKED. Only a click
     * that was handled and sent nothing gets the BN_CLICKED by hand. A fixed
     * wait sent it to clicks that were merely queued behind a busy game, and
     * the button fired twice: two Start Game presses hung a skirmish while it
     * looked for start positions for players that were not there. */
    DWORD tid = GetWindowThreadProcessId(d, NULL);
    g_watch_btn = c, g_watch_dlg = d, g_watch_ctrl = ctrl;
    LONG up0 = g_watch_up, cmd0 = g_watch_cmd;
    HHOOK hg = SetWindowsHookExA(WH_GETMESSAGE, watch_getmsg, NULL, tid);
    HHOOK hc = SetWindowsHookExA(WH_CALLWNDPROC, watch_callwnd, NULL, tid);
    click(c, MAKELPARAM(local.x, local.y));
    const char* how = NULL;
    DWORD t_up = 0;
    for (DWORD t0 = GetTickCount(); !how; Sleep(100)) {
        if (!t_up && g_watch_up != up0) t_up = GetTickCount();
        if (!IsWindow(d) || g_dialogs_opened != opens || g_watch_cmd != cmd0) how = "click";
        else if (!t_up && GetTickCount() - t0 > 120000) how = "click, never taken";
        else if (t_up && GetTickCount() - t_up > 2000) {
            PostMessageA(GetParent(c), WM_COMMAND, MAKEWPARAM(ctrl, BN_CLICKED), (LPARAM)c);
            how = "BN_CLICKED";
        }
    }
    UnhookWindowsHookEx(hg);
    UnhookWindowsHookEx(hc);
    return how;
}

/* Pick item N of a list or combo box: set the selection and send the parent
 * the notification a click on the item sends (LBN_SELCHANGE / CBN_SELCHANGE),
 * so the dialog procedure reacts as it would to the player. */
static const char* select_item(int dlg, int ctrl, int n, LONG* shift) {
    DWORD w0 = GetTickCount();
    HWND d;
    char cls[32] = "";
    while (!(d = find_dialog(dlg)) && GetTickCount() - w0 < 600000) Sleep(100);
    *shift += (LONG)(GetTickCount() - w0);
    if (!d) return NULL;
    Sleep(1500);
    HWND c = GetDlgItem(d, ctrl);
    if (!c) return "no such control";
    GetClassNameA(c, cls, sizeof cls);
    if (!_stricmp(cls, TRACKBAR_CLASSA)) {     /* a slider: N is its position */
        SendMessageA(c, TBM_SETPOS, TRUE, n);
        PostMessageA(GetParent(c), (GetWindowLongA(c, GWL_STYLE) & TBS_VERT) ? WM_VSCROLL : WM_HSCROLL,
                     MAKEWPARAM(TB_ENDTRACK, 0), (LPARAM)c);
        return "slider";
    }
    int combo = !_stricmp(cls, "ComboBox");
    SendMessageA(c, combo ? CB_SETCURSEL : LB_SETCURSEL, n, 0);
    PostMessageA(d, WM_COMMAND, MAKEWPARAM(ctrl, combo ? CBN_SELCHANGE : LBN_SELCHANGE), (LPARAM)c);
    return combo ? "combo" : "list";
}

static int cmp_ev(const void* a, const void* b) {
    double d = ((const ev_t*)a)->t - ((const ev_t*)b)->t;
    return d < 0 ? -1 : d > 0;
}

static DWORD WINAPI script(LPVOID unused) {
    (void)unused;
    while (!g_menu_open) Sleep(50);
    LONG shift = 0;                     /* ms the script runs late: waits */
    for (int i = 0; i < g_nev; i++) {
        ev_t* e = &g_ev[i];
        LONG wait = (LONG)(e->t * 1000) + shift - (LONG)(GetTickCount() - g_t0);
        if (wait > 0) Sleep(wait);
        DWORD prev_step = g_step_t0;
        g_step_t0 = GetTickCount();
        HWND h = g_input_hwnd;
        if (e->kind == 'p') {
            const char* how = press(e->x, e->y, &shift);
            fprintf(stderr, "[input] %.1fs press 0x%X:%d -> %s\n", e->t, e->x, e->y,
                    how ? how : "dialog never opened");
            continue;
        }
        if (e->kind == 's') {
            const char* how = select_item(e->x, e->y & 0xFFFF, e->y >> 16, &shift);
            fprintf(stderr, "[input] %.1fs select 0x%X:%d=%d -> %s\n", e->t, e->x, e->y & 0xFFFF,
                    e->y >> 16, how ? how : "dialog never opened");
            continue;
        }
        if (e->kind == 'l') {
            DWORD w0 = GetTickCount();
            InterlockedExchange(&g_saw_log, seen_since(e->text, prev_step));
            g_want_log = e->text;
            while (!g_saw_log && GetTickCount() - w0 < 600000) Sleep(50);
            g_want_log = NULL;
            shift += (LONG)(GetTickCount() - w0);
            fprintf(stderr, "[input] %.1fs waited %.1fs for log \"%s\"%s\n", e->t,
                    (GetTickCount() - w0) / 1000.0, e->text, g_saw_log ? "" : " -- never printed");
            continue;
        }
        if (e->kind == 'w') {
            DWORD w0 = GetTickCount();
            while (!*(volatile uint32_t*)(uintptr_t)e->x && GetTickCount() - w0 < 300000) Sleep(100);
            shift += (LONG)(GetTickCount() - w0);
            fprintf(stderr, "[input] %.1fs waited %.1fs for [0x%X] = %u\n", e->t,
                    (GetTickCount() - w0) / 1000.0, e->x, *(volatile uint32_t*)(uintptr_t)e->x);
            continue;
        }
        if (e->kind == 'k') {
            static const int mvk[] = { VK_CONTROL, VK_SHIFT, VK_MENU };
            fprintf(stderr, "[input] %.1fs key %s%s%s0x%X\n", e->t, e->y & 1 ? "Ctrl-" : "",
                    e->y & 2 ? "Shift-" : "", e->y & 4 ? "Alt-" : "", e->x);
            for (int m = 0; m < 3; m++)
                if (e->y & 1 << m) PostMessageA(h, WM_KEYDOWN, mvk[m], 1);
            InterlockedExchange(&g_mods, e->y);
            PostMessageA(h, WM_KEYDOWN, e->x, 1);
            Sleep(150);
            PostMessageA(h, WM_KEYUP, e->x, 0xC0000001);
            Sleep(500);         /* ponytail: the handler reads the modifiers when it runs; a busy frame longer than this would miss them */
            InterlockedExchange(&g_mods, 0);
            for (int m = 0; m < 3; m++)
                if (e->y & 1 << m) PostMessageA(h, WM_KEYUP, mvk[m], 0xC0000001);
            continue;
        }
        if (e->kind == 'd') {
            int x2 = e->mods >> 16, y2 = (int16_t)(e->mods & 0xFFFF);
            fprintf(stderr, "[input] %.1fs drag %d,%d to %d,%d\n", e->t, e->x, e->y, x2, y2);
            InterlockedExchange(&g_cx, e->x);
            InterlockedExchange(&g_cy, e->y);
            input_post_mouse(h, WM_MOUSEMOVE, 0, MAKELPARAM(e->x, e->y));
            Sleep(100);
            InterlockedOr(&g_mods, 8);
            input_post_mouse(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(e->x, e->y));
            lb_seen();
            for (int k = 1; k <= 10; k++) {         /* across, a step a frame or so */
                int x = e->x + (x2 - e->x) * k / 10, y = e->y + (y2 - e->y) * k / 10;
                InterlockedExchange(&g_cx, x);
                InterlockedExchange(&g_cy, y);
                input_post_mouse(h, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, y));
                Sleep(60);
            }
            lb_seen();
            input_post_mouse(h, WM_LBUTTONUP, 0, MAKELPARAM(x2, y2));
            InterlockedAnd(&g_mods, ~8);
            lb_seen();
            continue;
        }
        LPARAM lp = MAKELPARAM(e->x, e->y);
        fprintf(stderr, "[input] %.1fs %s %s%d,%d\n", e->t, e->kind == 'c' ? "click" : "move",
                e->mods & 1 ? "Ctrl-" : e->mods & 2 ? "Shift-" : e->mods & 4 ? "Alt-" : "", e->x, e->y);
        InterlockedExchange(&g_cx, e->x);
        InterlockedExchange(&g_cy, e->y);
        input_post_mouse(h, WM_MOUSEMOVE, 0, lp);
        if (e->kind == 'c') {
            static const int mvk[] = { VK_CONTROL, VK_SHIFT, VK_MENU };
            for (int m = 0; m < 3; m++)
                if (e->mods & 1 << m) PostMessageA(h, WM_KEYDOWN, mvk[m], 1);
            InterlockedOr(&g_mods, e->mods);
            Sleep(100);
            click(h, lp);
            InterlockedAnd(&g_mods, ~e->mods);
            for (int m = 0; m < 3; m++)
                if (e->mods & 1 << m) PostMessageA(h, WM_KEYUP, mvk[m], 0xC0000001);
        }
    }
    fprintf(stderr, "[input] script done\n");
    return 0;
}

HWND g_input_hwnd;
volatile LONG g_dialogs_opened;

void input_start(void) {
    InitializeCriticalSection(&g_dlg_lock);
    CloseHandle(CreateThread(NULL, 0, dialog_poller, NULL, 0, NULL));
    if (!g_nev) return;
    qsort(g_ev, g_nev, sizeof *g_ev, cmp_ev);
    CloseHandle(CreateThread(NULL, 0, script, NULL, 0, NULL));
}

int input_scripted(void) { return g_nev > 0; }
