/*
 * Battle info (docs/battle-info.md): health bars beyond the selection, the
 * build percentage on a cameo and the build time in its tooltip, and the
 * numbers the presenter's overlay line shows (game clock, frames a second,
 * orders a minute). The lift's patches call in here (run_lift.py
 * battle_info_patches); every setting is off by default, which is the game as
 * it shipped.
 *
 *   [present] healthbars=selected|damaged|always   (ra2.ini, F10)
 *             buildinfo=0|1
 *             overlay=0|1
 *
 * --healthbars MODE, --buildinfo and --overlay set them for a headless run.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "battleinfo.h"
#include "recomp_target.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* Windows only: the guest's memory is the host's (src/linux/main.c has the
 * patches' symbols, all off). */
#define GUEST_PTR(va) ((void *)(uintptr_t)(uint32_t)(va))

int ra2_healthbars;                  /* BATTLEINFO_BARS_*: read by the patch on every object drawn */
int ra2_buildinfo;
volatile int ra2_overlay;
static int g_log;                    /* a headless run asked for a setting: log what it did */

static const char* const k_bars[] = { "selected", "damaged", "always" };

int battleinfo_bars_from_name(const char* name) {
    for (int i = 0; i < 3; i++)
        if (!_stricmp(name, k_bars[i])) return i;
    return -1;
}

const char* battleinfo_bars_name(int mode) {
    return k_bars[mode >= 0 && mode < 3 ? mode : 0];
}

/* Set on the command line: ra2.ini doesn't override it. */
int battleinfo_cmdline(void) {
    return g_log;
}

int battleinfo_arg(int argc, char** argv, int i) {
    if (!strcmp(argv[i], "--healthbars") && i + 1 < argc && battleinfo_bars_from_name(argv[i + 1]) >= 0) {
        ra2_healthbars = battleinfo_bars_from_name(argv[i + 1]);
        g_log = 1;
        return 2;
    }
    if (!strcmp(argv[i], "--buildinfo")) { ra2_buildinfo = 1, g_log = 1; return 1; }
    if (!strcmp(argv[i], "--overlay")) { ra2_overlay = 1, g_log = 1; return 1; }
    return 0;
}

/* ---- health bars -----------------------------------------------------------
 * The game draws an object's bar when it is selected, or when the mouse is
 * over it and it is not. The patch asks here, for an object that is neither,
 * whether to take the mouse-over path anyway. */
static volatile long g_bars_forced;

int ra2_healthbar_force(int health, int strength) {
    int show = ra2_healthbars == BATTLEINFO_BARS_ALWAYS ||
               (ra2_healthbars == BATTLEINFO_BARS_DAMAGED && health < strength);
    if (show && !g_bars_forced++ && g_log)
        fprintf(stderr, "[battleinfo] first health bar shown by healthbars=%s\n", battleinfo_bars_name(ra2_healthbars));
    return show;
}

/* ---- build percentage and build time ---------------------------------------
 * The cameo being built shows its percentage where a finished one shows
 * "Ready": the patch hands the game this text in place of none, and the game
 * draws it with its own font, in its own box. Stage 54 of 54 is done. */
static wchar_t g_pct[8];
static volatile long g_pct_drawn;

uint32_t ra2_build_pct_text(int stage) {
    if (stage < 0) stage = 0;
    if (stage > 54) stage = 54;
    swprintf(g_pct, sizeof g_pct / sizeof g_pct[0], L"%d%%", stage * 100 / 54);
    if (!g_pct_drawn++ && g_log) fprintf(stderr, "[battleinfo] first build percentage: %ls\n", g_pct);
    return (uint32_t)(uintptr_t)g_pct;
}

/* The tooltip of a cameo, "Name\n$cost" in the game's own buffer, gains the
 * build time: the type's own (cost, the rules' BuildSpeed and its
 * BuildTimeMultiplier), in game seconds, 15 frames each, as at normal speed.
 * Low power and extra factories change the real time; this is the list
 * price. The game turns spaces in the buffer into line breaks, so none. */
void ra2_tooltip_time(uint32_t buffer, int frames) {
    static int logged;
    wchar_t* text = (wchar_t*)GUEST_PTR(buffer);
    size_t n = wcslen(text);
    int s = frames > 0 ? (frames + 14) / 15 : 0;
    if (!ra2_buildinfo || n + 12 >= 350) return;   /* 700 bytes to the sidebar's buttons */
    swprintf(text + n, 12, L"\n%d:%02d", s / 60, s % 60);
    if (g_log && logged < 5) {
        logged++;
        fprintf(stderr, "[battleinfo] tooltip build time %d:%02d (%d frames)\n", s / 60, s % 60, frames);
    }
}

/* ---- the overlay's numbers ---------------------------------------------------
 * Orders are the events this player puts in the game's outgoing list (the
 * patch is on every inline add): move, attack, build, place, deploy... Kept
 * per second for a minute, so orders a minute is the last 60 seconds'. */
static volatile long g_orders;

void ra2_order_added(void) {
    if (!g_orders++ && g_log) fprintf(stderr, "[battleinfo] first order counted\n");
}

#define WINDOW_S 60
static struct {
    long orders[WINDOW_S];           /* orders in each second of the last minute */
    int at, filled;
    long orders_seen;
    uint32_t frame_seen;
    int fps, apm, clock_s;
    int in_battle;
} g_stats;

/* Once a second from the presenter, or the host's ticker when headless:
 * the game's own frame count (it starts again with each game) gives the
 * clock and frames a second; the order count, orders a minute. */
void battleinfo_tick(void) {
    uint32_t frame = *(volatile uint32_t*)GUEST_PTR(RA2_GAME_FRAME_VA);
    long orders = g_orders;
    int moved = frame != g_stats.frame_seen;
    if (frame < g_stats.frame_seen) {              /* a new game: start over */
        memset(g_stats.orders, 0, sizeof g_stats.orders);
        g_stats.filled = 0;
    }
    g_stats.fps = frame >= g_stats.frame_seen ? (int)(frame - g_stats.frame_seen) : 0;
    g_stats.frame_seen = frame;
    g_stats.at = (g_stats.at + 1) % WINDOW_S;
    g_stats.orders[g_stats.at] = orders - g_stats.orders_seen;
    g_stats.orders_seen = orders;
    if (g_stats.filled < WINDOW_S) g_stats.filled++;
    long sum = 0;
    for (int i = 0; i < WINDOW_S; i++) sum += g_stats.orders[i];
    g_stats.apm = (int)(g_stats.filled < WINDOW_S ? sum * 60 / g_stats.filled : sum);
    g_stats.clock_s = (int)(frame / 15);
    g_stats.in_battle = moved;
    if (g_log) {
        static int n;
        if (moved && ++n % 10 == 0)
            fprintf(stderr, "[battleinfo] overlay %s; health bars %s, %ld forced; build %% drawn %ld times; "
                    "%ld orders\n", battleinfo_overlay_text(), battleinfo_bars_name(ra2_healthbars),
                    g_bars_forced, g_pct_drawn, orders);
    }
}

/* What the overlay line says, "12:34  60 FPS  85 APM", or "" outside a battle. */
const char* battleinfo_overlay_text(void) {
    static char line[64];
    if (!g_stats.in_battle) return "";
    _snprintf(line, sizeof line - 1, "%d:%02d  %d FPS  %d APM", g_stats.clock_s / 60, g_stats.clock_s % 60,
              g_stats.fps, g_stats.apm);
    line[sizeof line - 1] = 0;
    return line;
}

static DWORD WINAPI ticker(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(1000);
        battleinfo_tick();
    }
    return 0;
}

/* Headless there is no presenter to tick: a thread of its own, only when a
 * run asked for battle info (the playtests' logged check). */
void battleinfo_start_headless(void) {
    if (g_log) CloseHandle(CreateThread(NULL, 0, ticker, NULL, 0, NULL));
}
