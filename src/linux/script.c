/*
 * Scripted input for headless runs on the native Linux host: the Windows
 * host's grammar (src/runtime/input.c, docs/testing.md), so tools/playtest.py
 * drives both with the same cases.
 *
 *   --press DLG:CTRL@s   press control CTRL of menu dialog DLG, once DLG is open
 *   --select DLG:CTRL=N@s  pick item N of a list or combo box (a slider's position)
 *   --waitlog TEXT@s     hold until the game's debug log prints a line with TEXT
 *   --move x,y@s  --click [c][s][a]+x,y@s  --key [c][s][a]+vk@s  --wait VA@s
 *   --drag x1,y1,x2,y2@s
 *
 * s is seconds after the main menu's dialog (0xE2) opened; events run in time order on their
 * own thread, and a wait moves every later event back by however long it
 * took. Input is posted as messages to the game's windows, and the key and
 * cursor state its GetKeyState and GetCursorPos read is the script's. Every
 * touch of the window manager is made holding the machine (mach_enter), as
 * the game's own threads do.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include "win32hle.h"

#define WM_COMMAND     0x0111u
#define WM_HSCROLL     0x0114u
#define WM_VSCROLL     0x0115u
#define WM_KEYDOWN     0x0100u
#define WM_KEYUP       0x0101u
#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define MK_LBUTTON     1u

typedef struct { char kind; int x, y, mods; double t; char text[64]; } ev_t;
#define MAX_EV 256
static ev_t g_ev[MAX_EV];
static int g_nev;

static uint32_t now_ms(void) { return hle_ticks_ms(); }
static void sleep_ms(uint32_t ms) { usleep(ms * 1000u); }

int script_arg(int argc, char **argv, int i) {
    if (i + 1 >= argc || g_nev >= MAX_EV) return 0;
    ev_t e;
    memset(&e, 0, sizeof e);
    const char *a = argv[i + 1];
    if (!strcmp(argv[i], "--move") || !strcmp(argv[i], "--click")) {
        const char *plus = strchr(a, '+');
        if (plus) {
            for (const char *m = a; m < plus; m++) e.mods |= *m == 'c' ? 1 : *m == 's' ? 2 : *m == 'a' ? 4 : 0;
            a = plus + 1;
        }
        if (sscanf(a, "%d,%d@%lf", &e.x, &e.y, &e.t) != 3) return 0;
        e.kind = argv[i][2];
    } else if (!strcmp(argv[i], "--drag")) {
        int x2, y2;
        if (sscanf(a, "%d,%d,%d,%d@%lf", &e.x, &e.y, &x2, &y2, &e.t) != 5) return 0;
        e.mods = x2 << 16 | (y2 & 0xFFFF);
        e.kind = 'd';
    } else if (!strcmp(argv[i], "--press")) {
        if (sscanf(a, "%i:%i@%lf", &e.x, &e.y, &e.t) != 3) return 0;
        e.kind = 'p';
    } else if (!strcmp(argv[i], "--select")) {
        int n;
        if (sscanf(a, "%i:%i=%d@%lf", &e.x, &e.y, &n, &e.t) != 4) return 0;
        e.y |= n << 16;
        e.kind = 's';
    } else if (!strcmp(argv[i], "--waitlog")) {
        const char *at = strrchr(a, '@');
        if (!at || at - a >= (int)sizeof e.text || sscanf(at + 1, "%lf", &e.t) != 1) return 0;
        memcpy(e.text, a, (size_t)(at - a));
        e.kind = 'l';
    } else if (!strcmp(argv[i], "--wait")) {
        if (sscanf(a, "%i@%lf", &e.x, &e.t) != 2) return 0;
        e.kind = 'w';
    } else if (!strcmp(argv[i], "--key")) {
        const char *plus = strchr(a, '+');
        if (plus) {
            for (const char *m = a; m < plus; m++) e.y |= *m == 'c' ? 1 : *m == 's' ? 2 : *m == 'a' ? 4 : 0;
            a = plus + 1;
        }
        if (sscanf(a, "%i@%lf", &e.x, &e.t) != 2) return 0;
        e.kind = 'k';
    } else {
        return 0;
    }
    g_ev[g_nev++] = e;
    return 2;
}
int script_active(void) { return g_nev > 0; }

/* ---- dialogs: what is open, by resource ID ---- */
#define MAX_DLG 32
static struct { uint32_t h; int id; } g_dlg[MAX_DLG];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_dialogs_opened;
static volatile int g_menu_open;                 /* the main menu (0xE2) has opened: the script's clock starts */
static volatile uint32_t g_main_menu;            /* its window, the last time it opened */
static uint32_t g_t0;

/* user32's dialog hook: on the game's thread, holding the machine */
static void dialog_created(uint32_t h, uint32_t id) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_DLG; i++)
        if (!g_dlg[i].h || !hle_is_window(g_dlg[i].h)) { g_dlg[i].h = h, g_dlg[i].id = (int)id; break; }
    pthread_mutex_unlock(&g_lock);
    __sync_fetch_and_add(&g_dialogs_opened, 1);
    fprintf(stderr, "[dialog] open 0x%X\n", id);
    if (id == 0xE2 && !__sync_lock_test_and_set(&g_menu_open, 1)) g_t0 = now_ms();
    if (id == 0xE2) g_main_menu = h;
}

/* Whether the main menu is up: nothing to lose by restarting (mods.c). */
int script_at_main_menu(void) { return g_main_menu && hle_is_window(g_main_menu); }
static uint32_t find_dialog(int id) {
    uint32_t h = 0;
    mach_enter();
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_DLG && !h; i++)
        if (g_dlg[i].id == id && g_dlg[i].h && hle_is_window(g_dlg[i].h)) h = g_dlg[i].h;
    pthread_mutex_unlock(&g_lock);
    mach_leave();
    return h;
}
static void *dialog_poller(void *unused) {
    (void)unused;
    for (;;) {
        sleep_ms(100);
        mach_enter();
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < MAX_DLG; i++)
            if (g_dlg[i].h && !hle_is_window(g_dlg[i].h)) {
                fprintf(stderr, "[dialog] closed 0x%X\n", g_dlg[i].id);
                g_dlg[i].h = 0;
            }
        pthread_mutex_unlock(&g_lock);
        mach_leave();
    }
    return NULL;
}

/* ---- the game's debug log ---- */
#define RING 64
static struct { uint32_t t; char s[160]; } g_ring[RING];
static volatile int g_ring_n;
static uint32_t g_step_t0;
static const char *volatile g_want_log;
static volatile int g_saw_log;

void script_log_line(const char *line) {
    pthread_mutex_lock(&g_lock);
    int k = g_ring_n++;
    g_ring[k % RING].t = now_ms();
    snprintf(g_ring[k % RING].s, sizeof g_ring[0].s, "%s", line);
    const char *w = g_want_log;
    if (w && strstr(line, w)) g_saw_log = 1;
    pthread_mutex_unlock(&g_lock);
}
static int seen_since(const char *text, uint32_t since) {
    int found = 0;
    pthread_mutex_lock(&g_lock);
    for (int k = g_ring_n - 1; k >= 0 && k >= g_ring_n - RING && !found; k--)
        if ((int32_t)(g_ring[k % RING].t - since) >= 0 && strstr(g_ring[k % RING].s, text)) found = 1;
    pthread_mutex_unlock(&g_lock);
    return found;
}

/* ---- posting ---- */
static uint32_t game_window(void) { mach_enter(); uint32_t h = hle_first_hwnd(); mach_leave(); return h; }
static void post(uint32_t h, uint32_t m, uint32_t w, uint32_t l) { mach_enter(); hle_post_message(h, m, w, l); mach_leave(); }
static void key_state(uint32_t vk, int down) { mach_enter(); hle_input_key_state(vk, down); mach_leave(); }
static void cursor(int x, int y) { mach_enter(); hle_input_cursor(x, y); mach_leave(); }
static uint32_t lp_of(int x, int y) { return (uint32_t)(y & 0xFFFF) << 16 | (uint32_t)(x & 0xFFFF); }

/* until the game has read the left button twice more, up to 1 s */
static void lb_seen(void) {
    int r0 = hle_lbutton_reads;
    for (int i = 0; i < 40 && hle_lbutton_reads - r0 < 2; i++) sleep_ms(25);
}
/* held until the game has seen it down, then up */
static void click(uint32_t h, uint32_t lp) {
    key_state(1, 1);
    post(h, WM_LBUTTONDOWN, MK_LBUTTON, lp);
    sleep_ms(250);
    lb_seen();
    post(h, WM_LBUTTONUP, 0, lp);
    key_state(1, 0);
    lb_seen();
}

/* press(): the button's own button-up taken off the queue, and its BN_CLICKED */
static volatile uint32_t g_watch_btn;
static volatile int g_watch_ctrl, g_watch_up, g_watch_cmd;
static void watch(uint32_t h, uint32_t m, uint32_t w, uint32_t l) {
    if (!g_watch_btn) return;
    if (m == WM_LBUTTONUP && h == g_watch_btn) g_watch_up++;
    if (m == WM_COMMAND && l == g_watch_btn && (int)(w & 0xFFFF) == g_watch_ctrl && (w >> 16) == 0) g_watch_cmd++;
}

static const char *press(int dlg, int ctrl, int32_t *shift) {
    uint32_t w0 = now_ms(), d;
    while (!(d = find_dialog(dlg)) && now_ms() - w0 < 600000) sleep_ms(100);
    *shift += (int32_t)(now_ms() - w0);
    if (!d) return NULL;
    sleep_ms(1500);                                      /* the menus slide their buttons in */
    mach_enter();
    uint32_t c = hle_dialog_item(d, (uint32_t)ctrl);
    int l = 0, t = 0, r = 0, b = 0, ml = 0, mt = 0, mr, mb;
    uint32_t game = hle_first_hwnd(), target = c;
    if (c) {
        hle_window_screen_rect(c, &l, &t, &r, &b);
        hle_window_screen_rect(game, &ml, &mt, &mr, &mb);
        if (!strcasecmp(hle_window_class(c), "Static")) target = d;   /* statics pass clicks through */
    }
    mach_leave();
    if (!c) return "no such control";
    int mx = (l + r) / 2, my = (t + b) / 2, tl = 0, tt = 0, tr, tb;
    mach_enter();
    hle_window_screen_rect(target, &tl, &tt, &tr, &tb);
    mach_leave();
    uint32_t lp = lp_of(mx - tl, my - tt);
    cursor(mx - ml, my - mt);
    post(target, WM_MOUSEMOVE, 0, lp);
    sleep_ms(200);
    int opens = g_dialogs_opened;
    int up0 = g_watch_up, cmd0 = g_watch_cmd;
    g_watch_ctrl = ctrl;
    g_watch_btn = target;
    click(target, lp);
    const char *how = NULL;
    uint32_t t_up = 0;
    for (uint32_t t0 = now_ms(); !how; sleep_ms(100)) {
        if (!t_up && g_watch_up != up0) t_up = now_ms();
        mach_enter();
        int alive = hle_is_window(d);
        mach_leave();
        if (!alive || g_dialogs_opened != opens || g_watch_cmd != cmd0) how = "click";
        else if (!t_up && now_ms() - t0 > 120000) { how = "click, never taken"; mach_enter(); hle_queue_dump(); mach_leave(); }
        else if (t_up && now_ms() - t_up > 2000) {
            mach_enter();
            uint32_t parent = hle_window_parent(c);
            mach_leave();
            post(parent, WM_COMMAND, (uint32_t)ctrl, c);
            how = "BN_CLICKED";
        }
    }
    g_watch_btn = 0;
    return how;
}

static const char *select_item(int dlg, int ctrl, int n, int32_t *shift) {
    uint32_t w0 = now_ms(), d;
    while (!(d = find_dialog(dlg)) && now_ms() - w0 < 600000) sleep_ms(100);
    *shift += (int32_t)(now_ms() - w0);
    if (!d) return NULL;
    sleep_ms(1500);
    mach_enter();
    uint32_t c = hle_dialog_item(d, (uint32_t)ctrl);
    const char *how = "no such control";
    if (c) {
        const char *cls = hle_window_class(c);
        if (!strcasecmp(cls, "msctls_trackbar32")) {
            hle_send(c, 0x0405u, 1, (uint32_t)n);         /* TBM_SETPOS */
            hle_post_message(hle_window_parent(c), (hle_window_style(c) & 2u) ? WM_VSCROLL : WM_HSCROLL, 8u /* TB_ENDTRACK */, c);
            how = "slider";
        } else {
            int combo = !strcasecmp(cls, "ComboBox");
            hle_send(c, combo ? 0x014Eu : 0x0186u, (uint32_t)n, 0);   /* CB_ / LB_SETCURSEL */
            hle_post_message(d, WM_COMMAND, (uint32_t)ctrl | 1u << 16, c);   /* CBN_ / LBN_SELCHANGE */
            how = combo ? "combo" : "list";
        }
    }
    mach_leave();
    return how;
}

static int cmp_ev(const void *a, const void *b) {
    double d = ((const ev_t *)a)->t - ((const ev_t *)b)->t;
    return d < 0 ? -1 : d > 0;
}

static void *script(void *unused) {
    static const uint32_t mvk[] = { 0x11, 0x10, 0x12 };  /* Ctrl, Shift, Alt */
    (void)unused;
    while (!g_menu_open) sleep_ms(50);
    int32_t shift = 0;
    for (int i = 0; i < g_nev; i++) {
        ev_t *e = &g_ev[i];
        int32_t wait = (int32_t)(e->t * 1000) + shift - (int32_t)(now_ms() - g_t0);
        if (wait > 0) sleep_ms((uint32_t)wait);
        uint32_t prev_step = g_step_t0;
        g_step_t0 = now_ms();
        uint32_t h = game_window();
        if (e->kind == 'p') {
            const char *how = press(e->x, e->y, &shift);
            fprintf(stderr, "[input] %.1fs press 0x%X:%d -> %s\n", e->t, e->x, e->y, how ? how : "dialog never opened");
            continue;
        }
        if (e->kind == 's') {
            const char *how = select_item(e->x, e->y & 0xFFFF, e->y >> 16, &shift);
            fprintf(stderr, "[input] %.1fs select 0x%X:%d=%d -> %s\n", e->t, e->x, e->y & 0xFFFF, e->y >> 16,
                    how ? how : "dialog never opened");
            continue;
        }
        if (e->kind == 'l') {
            uint32_t w0 = now_ms();
            g_saw_log = seen_since(e->text, prev_step);
            g_want_log = e->text;
            while (!g_saw_log && now_ms() - w0 < 600000) sleep_ms(50);
            g_want_log = NULL;
            shift += (int32_t)(now_ms() - w0);
            fprintf(stderr, "[input] %.1fs waited %.1fs for log \"%s\"%s\n", e->t, (now_ms() - w0) / 1000.0, e->text,
                    g_saw_log ? "" : " -- never printed");
            continue;
        }
        if (e->kind == 'w') {
            uint32_t w0 = now_ms();
            while (!MEM32((uint32_t)e->x) && now_ms() - w0 < 300000) sleep_ms(100);
            shift += (int32_t)(now_ms() - w0);
            fprintf(stderr, "[input] %.1fs waited %.1fs for [0x%X] = %u\n", e->t, (now_ms() - w0) / 1000.0, e->x, MEM32((uint32_t)e->x));
            continue;
        }
        if (e->kind == 'k') {
            fprintf(stderr, "[input] %.1fs key %s%s%s0x%X\n", e->t, e->y & 1 ? "Ctrl-" : "", e->y & 2 ? "Shift-" : "",
                    e->y & 4 ? "Alt-" : "", e->x);
            for (int m = 0; m < 3; m++)
                if (e->y & 1 << m) { key_state(mvk[m], 1); post(h, WM_KEYDOWN, mvk[m], 1); }
            key_state((uint32_t)e->x, 1);
            post(h, WM_KEYDOWN, (uint32_t)e->x, 1);
            sleep_ms(150);
            key_state((uint32_t)e->x, 0);
            post(h, WM_KEYUP, (uint32_t)e->x, 0xC0000001u);
            sleep_ms(500);
            for (int m = 0; m < 3; m++)
                if (e->y & 1 << m) { key_state(mvk[m], 0); post(h, WM_KEYUP, mvk[m], 0xC0000001u); }
            continue;
        }
        if (e->kind == 'd') {
            int x2 = e->mods >> 16, y2 = (int16_t)(e->mods & 0xFFFF);
            fprintf(stderr, "[input] %.1fs drag %d,%d to %d,%d\n", e->t, e->x, e->y, x2, y2);
            cursor(e->x, e->y);
            post(h, WM_MOUSEMOVE, 0, lp_of(e->x, e->y));
            sleep_ms(100);
            key_state(1, 1);
            post(h, WM_LBUTTONDOWN, MK_LBUTTON, lp_of(e->x, e->y));
            lb_seen();
            for (int k = 1; k <= 10; k++) {
                int x = e->x + (x2 - e->x) * k / 10, y = e->y + (y2 - e->y) * k / 10;
                cursor(x, y);
                post(h, WM_MOUSEMOVE, MK_LBUTTON, lp_of(x, y));
                sleep_ms(60);
            }
            lb_seen();
            post(h, WM_LBUTTONUP, 0, lp_of(x2, y2));
            key_state(1, 0);
            lb_seen();
            continue;
        }
        fprintf(stderr, "[input] %.1fs %s %s%d,%d\n", e->t, e->kind == 'c' ? "click" : "move",
                e->mods & 1 ? "Ctrl-" : e->mods & 2 ? "Shift-" : e->mods & 4 ? "Alt-" : "", e->x, e->y);
        cursor(e->x, e->y);
        post(h, WM_MOUSEMOVE, 0, lp_of(e->x, e->y));
        if (e->kind == 'c') {
            for (int m = 0; m < 3; m++)
                if (e->mods & 1 << m) { key_state(mvk[m], 1); post(h, WM_KEYDOWN, mvk[m], 1); }
            sleep_ms(100);
            click(h, lp_of(e->x, e->y));
            for (int m = 0; m < 3; m++)
                if (e->mods & 1 << m) { key_state(mvk[m], 0); post(h, WM_KEYUP, mvk[m], 0xC0000001u); }
        }
    }
    fprintf(stderr, "[input] script done\n");
    return NULL;
}

void script_start(void) {
    pthread_t t;
    hle_dialog_hook = dialog_created;
    hle_proc_hook = watch;
    pthread_create(&t, NULL, dialog_poller, NULL);
    pthread_detach(t);
    if (!g_nev) return;
    qsort(g_ev, (size_t)g_nev, sizeof *g_ev, cmp_ev);
    pthread_create(&t, NULL, script, NULL);
    pthread_detach(t);
}
