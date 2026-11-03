/* Modern controls' orders (docs/modern-controls.md): Shift-queued moves, and
 * below the idle-unit keys and game speed and pause.
 *
 * Shift + right-click in a battle adds a point to the selected units' route.
 * The first point is the game's own move, given by the click as any other;
 * each later one waits here, per unit, and when the unit stops moving the
 * battlefield's frame hook (run_lift.py ORDERS_TARGETS) gives it the next
 * through the same order routine a click on the ground calls. So every leg is
 * an ordinary move order, with the game's own pathing, voices and events.
 * Game-thread code only: the hooks that call it run in the game's loop. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "input.h"
#include "recomp_target.h"

#define GUEST32(va) (*(volatile uint32_t*)(uintptr_t)(uint32_t)(va))

enum { MISSION_MOVE = 2, MISSION_QMOVE = 3, ACTION_MOVE = 1 };
#define ROUTE_UNITS  64
#define ROUTE_POINTS 16
/* A leg the unit was never seen moving on (the cell was its own, or out of
 * reach) counts as done after this many game frames. */
#define ROUTE_WAIT_FRAMES 45

typedef struct {
    uint32_t unit;              /* 0: a free slot */
    int leg;                    /* the point it is on, 1-based */
    int moving;                 /* seen on a Move since the leg's order */
    int wait;                   /* frames left to be seen moving */
    int n;                      /* points waiting */
    uint32_t cells[ROUTE_POINTS];   /* CellStruct (x | y << 16), next first */
} route_t;
static route_t g_route[ROUTE_UNITS];
static uint32_t g_click_cell;   /* the cell of the click being resolved */

/* The mission as the game's own getter answers it: the current one, or while
 * that is -1 (an order assigned, not yet begun) the queued one. */
static uint32_t mission_of(uint32_t unit) {
    uint32_t m = GUEST32(unit + RA2_ORDERS_MISSION_OFFSET);
    return m != 0xFFFFFFFFu ? m : GUEST32(unit + RA2_ORDERS_MISSION_OFFSET + 8);
}

static int moving(uint32_t unit) {
    uint32_t m = mission_of(unit);
    return m == MISSION_MOVE || m == MISSION_QMOVE;
}

static route_t* route_of(uint32_t unit) {
    for (int i = 0; i < ROUTE_UNITS; i++)
        if (g_route[i].unit == unit) return &g_route[i];
    return NULL;
}

static route_t* route_new(uint32_t unit) {
    route_t* r = route_of(unit);
    for (int i = 0; !r && i < ROUTE_UNITS; i++)
        if (!g_route[i].unit) r = &g_route[i];
    if (r) memset(r, 0, sizeof *r), r->unit = unit;
    return r;
}

static int shift_held(void) {
    return (input_key_state(VK_SHIFT, GetKeyState(VK_SHIFT)) & 0x8000) != 0;
}

/* From the battlefield mouse hook, before the click's action is resolved. */
void ra2_orders_click_cell(uint32_t cell) { g_click_cell = cell; }

/* A routed right-click in Modern controls (present.c), with the action the
 * game resolved: 1 if it was taken as a queued point (the click then does
 * nothing more), 0 to let it through. Without Shift, a new order replaces
 * the selected units' routes. */
int ra2_orders_right_click(uint32_t action) {
    uint32_t n = GUEST32(RA2_SELECTION_COUNT_VA), items = GUEST32(RA2_SELECTION_ITEMS_VA);
    int x = (int16_t)(g_click_cell & 0xFFFF), y = (int16_t)(g_click_cell >> 16);
    if (!n || !items || n > 4096) return 0;
    if (!shift_held()) {
        for (uint32_t i = 0; i < n; i++) {
            route_t* r = route_of(GUEST32(items + i * 4));
            if (r) r->unit = 0;
        }
        return 0;
    }
    if (action != ACTION_MOVE) return 0;
    /* Units already on a route, or moving from an order before it, take the
     * point after the ones they have; otherwise the click is the first point,
     * the game's own move, and starts new routes. */
    int all = 1;
    for (uint32_t i = 0; i < n && all; i++) {
        uint32_t u = GUEST32(items + i * 4);
        all = route_of(u) || moving(u);
    }
    if (!all) {
        for (uint32_t i = 0; i < n; i++) {
            route_t* r = route_new(GUEST32(items + i * 4));
            if (r) r->leg = 1, r->wait = ROUTE_WAIT_FRAMES;
        }
        fprintf(stderr, "[orders] route start at %d,%d: %u units\n", x, y, n);
        return 0;
    }
    int point = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t u = GUEST32(items + i * 4);
        route_t* r = route_of(u);
        if (!r && (r = route_new(u)) != NULL) r->leg = 1, r->moving = 1;
        if (!r || r->n == ROUTE_POINTS) continue;
        r->cells[r->n++] = g_click_cell;
        if (r->leg + r->n > point) point = r->leg + r->n;
    }
    fprintf(stderr, "[orders] route point %d at %d,%d: %u units\n", point, x, y, n);
    return 1;
}

/* Once a frame: drop the routes of units that are gone, and note which have
 * stopped. Gone means no longer in the game's list of technos, so a freed
 * object is never read. */
static int g_due[ROUTE_UNITS], g_ndue, g_idue;

static void routes_scan(void) {
    int live[ROUTE_UNITS], nlive = 0;
    g_ndue = g_idue = 0;
    for (int i = 0; i < ROUTE_UNITS; i++)
        if (g_route[i].unit) live[nlive++] = i;
    if (!nlive) return;
    uint32_t n = GUEST32(RA2_TECHNOS_COUNT_VA), items = GUEST32(RA2_TECHNOS_ITEMS_VA);
    char found[ROUTE_UNITS] = { 0 };
    for (uint32_t k = 0; items && k < n; k++) {
        uint32_t t = GUEST32(items + k * 4);
        for (int j = 0; j < nlive; j++)
            if (g_route[live[j]].unit == t) found[j] = 1;
    }
    for (int j = 0; j < nlive; j++) {
        route_t* r = &g_route[live[j]];
        if (!found[j]) { r->unit = 0; continue; }
        if (moving(r->unit)) { r->moving = 1; continue; }
        if (!r->moving && --r->wait > 0) continue;
        if (!r->n) {
            fprintf(stderr, "[orders] unit %08X: route done at point %d (mission %d, then %d %d %d, %s)\n", r->unit, r->leg,
                    (int)GUEST32(r->unit + RA2_ORDERS_MISSION_OFFSET), (int)GUEST32(r->unit + RA2_ORDERS_MISSION_OFFSET + 4),
                    (int)GUEST32(r->unit + RA2_ORDERS_MISSION_OFFSET + 8), (int)GUEST32(r->unit + RA2_ORDERS_MISSION_OFFSET + 12),
                    r->moving ? "moved" : "never seen moving");
            r->unit = 0;
            continue;
        }
        g_due[g_ndue++] = live[j];
    }
}

/* The frame hook asks for each order due this frame: the unit and the cell
 * to send it to, 0 when there are no more. */
int ra2_orders_next(uint32_t* unit, uint32_t* cell) {
    static uint32_t frame = 0xFFFFFFFFu;
    uint32_t f = GUEST32(RA2_GAME_FRAME_VA);
    if (f != frame) frame = f, routes_scan();
    if (g_idue >= g_ndue) return 0;
    route_t* r = &g_route[g_due[g_idue++]];
    *unit = r->unit;
    *cell = r->cells[0];
    memmove(r->cells, r->cells + 1, --r->n * sizeof r->cells[0]);
    r->leg++, r->moving = 0, r->wait = ROUTE_WAIT_FRAMES;
    fprintf(stderr, "[orders] unit %08X: point %d at %d,%d\n", r->unit, r->leg,
            (int16_t)(*cell & 0xFFFF), (int16_t)(*cell >> 16));
    return 1;
}

/* ---- idle keys ----------------------------------------------------------
 * [keys] idleharv= and idleunit= (Modern controls): select the next idle
 * harvester, or the next idle combat unit, and centre the view on it; again
 * goes on to the one after. The key is taken on the window thread; the frame
 * hook asks for it here and does the selecting with the game's own calls (the
 * shared selection clear, the object's Select(), the CenterView command). */
enum { MISSION_SLEEP = 0, MISSION_GUARD = 5 };
static volatile LONG g_idle_request;    /* 1: a harvester, 2: a combat unit */
static uint32_t g_idle_last[2];         /* the one picked last, per key */

void ra2_orders_idle_key(int harvester) {
    InterlockedExchange(&g_idle_request, harvester ? 1 : 2);
}

int ra2_orders_idle_request(void) { return (int)InterlockedExchange(&g_idle_request, 0); }

/* Idle: the player's own vehicle or infantry, standing (Sleep or Guard, where
 * a unit goes when it has nothing to do or was stopped). Harvesters are units
 * whose type says Harvester=yes; every other one is a combat unit here. */
uint32_t ra2_orders_idle_pick(int kind) {
    uint32_t n = GUEST32(RA2_TECHNOS_COUNT_VA), items = GUEST32(RA2_TECHNOS_ITEMS_VA);
    uint32_t player = GUEST32(RA2_CURRENT_PLAYER_VA), start = 0;
    if (!items || !n || n > 100000) return 0;
    for (uint32_t k = 0; k < n; k++)
        if (GUEST32(items + k * 4) == g_idle_last[kind - 1]) { start = k + 1; break; }
    for (uint32_t k = 0; k < n; k++) {
        uint32_t t = GUEST32(items + (start + k) % n * 4), vt = GUEST32(t);
        int unit = vt == RA2_MODERN_UNIT_VTABLE;
        if (!unit && vt != RA2_MODERN_INFANTRY_VTABLE) continue;
        if (GUEST32(t + RA2_TECHNO_OWNER_OFFSET) != player) continue;
        uint32_t m = mission_of(t);
        if (m != MISSION_SLEEP && m != MISSION_GUARD) continue;
        uint32_t type = unit ? GUEST32(t + RA2_UNIT_TYPE_OFFSET) : 0;
        int harvester = type && *(volatile uint8_t*)(uintptr_t)(type + RA2_UNITTYPE_HARVESTER_OFFSET);
        if (harvester != (kind == 1)) continue;
        return g_idle_last[kind - 1] = t;
    }
    fprintf(stderr, "[orders] no idle %s\n", kind == 1 ? "harvester" : "unit");
    return 0;
}

/* After its Select(): a refused one (in a transport, say) is passed over. */
void ra2_orders_idle_selected(int kind, uint32_t unit, int selected) {
    fprintf(stderr, "[orders] idle %s %08X %s\n", kind == 1 ? "harvester" : "unit", unit,
            selected ? "selected" : "refused");
}

/* ---- game speed and pause -------------------------------------------------
 * [keys] faster=, slower= and pause= (Modern controls), in single player only
 * (a campaign mission or a skirmish): never online, where the speed is the
 * session's and a pause would desync. The speed is the game's own option
 * (OptionsClass::GameSpeed, 0 fastest to 6 slowest), which the main loop
 * reads into its frame timer every pass, as the in-game options slider's
 * GAMESPEED event sets it. Pause skips the game's logic frame the way the
 * main loop's own end-of-game checks do (run_lift.py ORDERS_TARGETS 'loop'):
 * the picture, the scrolling and the menus go on, and orders given meanwhile
 * wait for the next frame. */
enum { SESSION_CAMPAIGN = 0, SESSION_SKIRMISH = 5, SPEED_SLOWEST = 6 };
static volatile LONG g_speed_step, g_pause_toggle;
static int g_paused;
static uint32_t g_paused_frame;

void ra2_orders_speed_key(int step) { InterlockedExchangeAdd(&g_speed_step, step); }
void ra2_orders_pause_key(void) { InterlockedExchange(&g_pause_toggle, 1); }

/* Every pass of the main loop, just before it decides whether to run a game
 * frame: 1 to skip it. */
int ra2_orders_loop(void) {
    LONG step = InterlockedExchange(&g_speed_step, 0), toggle = InterlockedExchange(&g_pause_toggle, 0);
    uint32_t session = GUEST32(RA2_SESSION_TYPE_VA), frame = GUEST32(RA2_GAME_FRAME_VA);
    if (session != SESSION_CAMPAIGN && session != SESSION_SKIRMISH) {
        if (step || toggle) fprintf(stderr, "[orders] speed and pause are for single player only\n");
        return g_paused = 0;
    }
    if (g_paused && frame != g_paused_frame) g_paused = 0;    /* a new game started */
    if (step) {
        int was = (int)GUEST32(RA2_GAME_SPEED_VA), now = was - (int)step;
        now = now < 0 ? 0 : now > SPEED_SLOWEST ? SPEED_SLOWEST : now;
        GUEST32(RA2_GAME_SPEED_VA) = (uint32_t)now;
        fprintf(stderr, "[orders] game speed %d -> %d\n", was, now);
    }
    if (toggle) {
        g_paused = !g_paused;
        if (g_paused) g_paused_frame = frame;
        fprintf(stderr, g_paused ? "[orders] paused at frame %u\n" : "[orders] resumed at frame %u\n", frame);
    }
    return g_paused;
}
