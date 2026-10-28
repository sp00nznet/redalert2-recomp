#ifndef RA2_WEB_H
#define RA2_WEB_H
/* The game folder (the browser's imported copy, else the server's) and the
 * folder for what the game writes: 1 if there is a game. */
int  web_mount(const char *exe_name, char *game_dir, size_t gn, char *save_dir, size_t sn);
void web_set_status(const char *text);                            /* a line for the page to show */
void web_pump(void);
#endif
