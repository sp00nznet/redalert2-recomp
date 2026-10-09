/* mods.c: a mod's folder laid over the game's, chosen with --mod or F9. */
#pragma once

/* Start playing a mod: requested (--mod; "none" for no mod) or else the one
 * remembered (ra2.ini), over game_dir. argc/argv are what a switch restarts
 * with; save writes ra2.ini with mods_active() in it. After the window is open. */
void mods_start(const char *game_dir, const char *requested, const char *remembered,
                int argc, char **argv, void (*save)(void));
const char *mods_active(void);   /* "" when none */
