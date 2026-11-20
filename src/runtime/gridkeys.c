/* Sidebar grid hotkeys, part of Modern controls (docs/modern-controls.md):
 * Alt + a key of a 3x4 grid does what a click on the cameo in that sidebar
 * slot does. Called from the lifted in-game message hook (run_lift.py
 * grid_keys_patches) with the window message's address, before the game's
 * keyboard handler sees it. */
#include <stdint.h>

#include "recomp_target.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "input.h"

int ra2_modern_controls(void);   /* present.c */

#define GUEST_PTR(va) ((void *)(uintptr_t)(uint32_t)(va))
#define GRID_KEYS 12
#define BUTTONS_PER_TAB 60u      /* the sidebar's static cameo buttons: four tabs of 60 */
#define BUTTON_SIZE 0x38u

enum { GRID_OFF, GRID_ALT, GRID_HOVER };
static const char* const k_grid_modes[] = { "off", "alt", "hover" };
static int  g_mode = -1;         /* -1: as ra2.ini says */
static int  g_loaded;
static int  g_forced;            /* --gridkeys: on without Modern controls (playtests are headless) */
static char g_grid[GRID_KEYS + 1] = "QWERASDFZXCV";

static int mode_from_name(const char* name) {
    for (int i = 0; i < 3; i++)
        if (!_stricmp(name, k_grid_modes[i])) return i;
    return -1;
}

/* --gridkeys alt|hover|off: the mode, whatever ra2.ini says, and grid keys on
 * whether or not Modern controls are. Returns 0 for an unknown mode. */
int ra2_gridkeys_arg(const char* mode) {
    int m = mode_from_name(mode);
    if (m < 0) return 0;
    g_mode = m, g_forced = 1;
    return 1;
}

/* ra2.ini beside ra2.exe, as the presenter keeps it: [present] gridkeys= and
 * [keys] grid= (twelve keys, row by row). */
static void load_settings(void) {
    char ini[MAX_PATH], v[32], *slash;
    GetModuleFileNameA(NULL, ini, MAX_PATH);
    slash = strrchr(ini, '\\');
    strcpy_s(slash ? slash + 1 : ini, MAX_PATH - (slash ? (slash + 1 - ini) : 0), "ra2.ini");
    GetPrivateProfileStringA("keys", "grid", "", v, sizeof v, ini);
    if (strlen(v) == GRID_KEYS) {
        for (int i = 0; i < GRID_KEYS; i++) g_grid[i] = (char)toupper((unsigned char)v[i]);
    } else if (v[0]) {
        fprintf(stderr, "[gridkeys] [keys] grid=%s ignored: it needs %d keys\n", v, GRID_KEYS);
    }
    if (g_mode < 0) {
        GetPrivateProfileStringA("present", "gridkeys", "alt", v, sizeof v, ini);
        g_mode = mode_from_name(v);
        if (g_mode < 0) {
            fprintf(stderr, "[gridkeys] gridkeys=%s unknown, using alt\n", v);
            g_mode = GRID_ALT;
        }
    }
}

static int held(int vk) { return (input_key_state(vk, GetKeyState(vk)) & 0x8000) != 0; }

/* The active tab's cameo buttons that are in the gadget list (LinkClass Next
 * or Prev set: the strip removes the others), in reading order: the sidebar
 * lays them out two to a row, so the grid's keys go left, right, next row. */
static int visible_slots(uint32_t* out, int max, int* tab) {
    uint32_t map = RA2_GRID_MAP_VA;
    int active = (int)*(volatile uint32_t*)GUEST_PTR(map + RA2_GRID_ACTIVE_TAB);
    if (active < 0 || active > 3) return 0;
    uint32_t set = *(volatile uint32_t*)GUEST_PTR(map + RA2_GRID_TABS + active * RA2_GRID_TAB_SIZE + RA2_GRID_TAB_SET);
    if (set > 3) return 0;
    int n = 0;
    for (uint32_t i = 0; i < BUTTONS_PER_TAB && n < max; i++) {
        uint32_t b = RA2_GRID_BUTTONS_VA + (set * BUTTONS_PER_TAB + i) * BUTTON_SIZE;
        if (*(volatile uint32_t*)GUEST_PTR(b + 4) || *(volatile uint32_t*)GUEST_PTR(b + 8)) out[n++] = b;
    }
    for (int i = 1; i < n; i++)                    /* by Y, then X */
        for (int j = i; j > 0; j--) {
            int32_t* a = (int32_t*)GUEST_PTR(out[j - 1]), *c = (int32_t*)GUEST_PTR(out[j]);
            if (a[4] < c[4] || (a[4] == c[4] && a[3] <= c[3])) break;
            uint32_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    *tab = active;
    return n;
}

void ra2_gridkey_message(uint32_t hwnd, uint32_t msg_va, uint32_t wparam_va, uint32_t lparam_va) {
    volatile uint32_t* msg = (volatile uint32_t*)GUEST_PTR(msg_va);
    if (*msg != WM_KEYDOWN && *msg != WM_SYSKEYDOWN) return;
    if (!g_loaded) load_settings(), g_loaded = 1;
    if (g_mode == GRID_OFF || (!g_forced && !ra2_modern_controls())) return;
    uint32_t vk = *(volatile uint32_t*)GUEST_PTR(wparam_va), lp = *(volatile uint32_t*)GUEST_PTR(lparam_va);
    const char* k = vk < 0x80 ? strchr(g_grid, (int)vk) : NULL;
    if (!k || !vk) return;
    int alt = (*msg == WM_SYSKEYDOWN && (lp & (1u << 29))) || held(VK_MENU);
    if (held(VK_CONTROL)) return;                  /* Ctrl+key stays the game's (teams, etc.) */

    uint32_t slots[BUTTONS_PER_TAB];
    int tab, n = visible_slots(slots, BUTTONS_PER_TAB, &tab);
    if (g_mode == GRID_ALT ? !alt : !n) return;
    if (g_mode == GRID_HOVER) {                    /* the pointer over the sidebar's cameos */
        POINT p;
        int32_t left = 0x7FFFFFFF;
        for (int i = 0; i < n; i++)
            if (((int32_t*)GUEST_PTR(slots[i]))[3] < left) left = ((int32_t*)GUEST_PTR(slots[i]))[3];
        if (!input_cursor(&p) || p.x < left - 16) return;
    }
    *msg = WM_NULL;                                /* the key is the grid's now, not the game's */
    if (lp & (1u << 30)) return;                   /* auto-repeat: one click per press */

    int slot = (int)(k - g_grid), right = held(VK_SHIFT);
    if (slot >= n) {
        fprintf(stderr, "[gridkeys] %c -> tab %d slot %d: not on the sidebar (%d slots)\n", (char)vk, tab, slot, n);
        return;
    }
    int32_t* g = (int32_t*)GUEST_PTR(slots[slot]);   /* GadgetClass: X, Y, Width, Height at +0xC */
    int x = g[3] + g[5] / 2, y = g[4] + g[6] / 2;
    /* What the mouse would post: the game's keyboard handler queues the click
     * with this point, and the sidebar's gadget takes it as its own. Shift
     * makes it a right click (hold, then cancel). */
    input_post_mouse((HWND)(uintptr_t)hwnd, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN,
                     right ? MK_RBUTTON : MK_LBUTTON, MAKELPARAM(x, y));
    input_post_mouse((HWND)(uintptr_t)hwnd, right ? WM_RBUTTONUP : WM_LBUTTONUP, 0, MAKELPARAM(x, y));
    fprintf(stderr, "[gridkeys] %s%c -> tab %d slot %d, %s click at %d,%d\n", alt ? "Alt+" : "",
            (char)vk, tab, slot, right ? "right" : "left", x, y);
}

#else

/* The native Linux and macOS host has no Modern controls (src/linux/main.c):
 * keys stay the game's. */
void ra2_gridkey_message(uint32_t hwnd, uint32_t msg_va, uint32_t wparam_va, uint32_t lparam_va) {
    (void)hwnd, (void)msg_va, (void)wparam_va, (void)lparam_va;
}

#endif
