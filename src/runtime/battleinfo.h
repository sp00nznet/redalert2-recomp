/* Battle info: health bars, build percentage and time, the overlay's numbers (battleinfo.c). */
#pragma once
#include <stdint.h>

enum { BATTLEINFO_BARS_SELECTED, BATTLEINFO_BARS_DAMAGED, BATTLEINFO_BARS_ALWAYS };

extern int ra2_healthbars;           /* BATTLEINFO_BARS_* */
extern int ra2_buildinfo;            /* build % on the cameo, build time in its tooltip */
extern volatile int ra2_overlay;     /* the presenter's clock / FPS / APM line */

int  battleinfo_arg(int argc, char** argv, int i);   /* argv entries taken, or 0 */
int  battleinfo_cmdline(void);                       /* a setting came from the command line */
int  battleinfo_bars_from_name(const char* name);    /* -1 if none */
const char* battleinfo_bars_name(int mode);
void battleinfo_tick(void);                          /* once a second */
const char* battleinfo_overlay_text(void);           /* "" outside a battle */
void battleinfo_start_headless(void);
