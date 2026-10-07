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

/* A mouse message the host posts to a game window, noted so that under Wine
 * the game thread's hook can tell it from the real mouse (host.c), and the
 * check that takes the note back. */
void  input_post_mouse(HWND h, UINT m, WPARAM w, LPARAM l);
int   input_posted_mouse(HWND h, UINT m, LPARAM l);
int   input_posted_mouse_peek(HWND h, UINT m, LPARAM l);   /* the same, keeping the note */                    /* FALSE: no scripted position yet */
SHORT input_key_state(int vk, SHORT real);
