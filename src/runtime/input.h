/* Scripted input for headless runs: input.c, docs/testing.md. */
#pragma once
#include <windows.h>

int  input_arg(int argc, char** argv, int i);   /* argv entries taken, or 0 */
void input_start(void);                          /* before entering the game */
int  input_scripted(void);

/* The host reports dialogs as the game creates them (resource ID from the
 * FindResourceA(RT_DIALOG) just before). */
void input_dialog_created(HWND h, int id);
extern volatile LONG g_dialogs_opened;
extern HWND g_input_hwnd;                        /* the game's main window */

/* The game's debug log, line by line, for --waitlog. */
int  input_wants_log(void);
void input_log_line(const char* line);

/* What the shimmed GetCursorPos / GetKeyState / GetAsyncKeyState answer. */
BOOL  input_cursor(POINT* p);
void  input_mode_changed(int w, int h);          /* keep the cursor on screen */

/* Live input from the presenter: the cursor in game coordinates, and key
 * state from the physical keyboard. */
void  input_live(int on);
void  input_live_cursor(int x, int y);
void  input_live_buttons(int mk);                /* MK_* buttons held */
int   input_live_button_state(void);              /* current MK_* buttons held */
void  input_live_map_right_to_left(int on);       /* Modern short right-click maps to left state */
void  input_live_swap_left_right(int on);         /* Modern minimap event uses the opposite button */

/* A mouse message the host posts to a game window, noted so that under Wine
 * the game thread's hook can tell it from the real mouse (host.c), and the
 * check that takes the note back. */
void  input_post_mouse(HWND h, UINT m, WPARAM w, LPARAM l);
int   input_posted_mouse(HWND h, UINT m, LPARAM l);
int   input_posted_mouse_peek(HWND h, UINT m, LPARAM l);   /* the same, keeping the note */                    /* FALSE: no scripted position yet */
SHORT input_key_state(int vk, SHORT real);
void  input_mode(int* w, int* h);                /* the game's display mode, last set */
void  input_selftest_live(int on);               /* --selftest-input: the buttons the presenter saw count */

/* ra2.ini [keys]: the presenter's rebindable keys, one row each in input.c's
 * k_keys. A key a later feature needs is an ID here and a row there; the F10
 * keybinding page lists the table. */
enum {
    KEY_ATTACKMOVE,     /* Modern controls: arm attack-move (present.c) */
    KEY_SETTINGS,       /* the F10 menu */
    KEY_FULLSCREEN,     /* F11 */
    KEY_SCALING,        /* F12: the next scaling mode */
    KEY_MOUSE4,         /* the key mouse button 4 (back) sends; 0: none */
    KEY_MOUSE5,         /* ... button 5 (forward) */
    KEY_COUNT
};
int         input_key(int id);                   /* the bound virtual key, 0 if none */
int         input_key_is(int id, int vk);
const char* input_key_ini_name(int id);          /* "attackmove" */
const char* input_key_label(int id);             /* "Attack-move (Modern)" */
void        input_key_set(int id, int vk);
int         input_key_from_name(const char* v);  /* "A", "F10", "0x41": the VK, 0 for none, -1 unknown */
void        input_key_to_name(int vk, char* out, int n);
void        input_keys_load(const char* ini);    /* [keys] from ra2.ini; --bind still wins */
