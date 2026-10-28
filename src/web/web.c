/*
 * web.c - what the browser build needs before the game starts: its files.
 *
 * The game's folder is the player's own, copied once by the page
 * (web/import.js) into the browser's Origin Private File System, game/,
 * which WASMFS's OPFS backend serves; no server ever holds or sends a game
 * file. What the game writes (settings, saves, the INIs it edits) goes to
 * save/ beside it, laid over the game folder as win32hle's overlay, so
 * nothing is lost on reload. The fonts come from beside the page, through
 * WASMFS's fetch backend.
 *
 * Runs on the game's thread (PROXY_TO_PTHREAD): WASMFS's fetch and OPFS
 * backends cannot be made on the browser's main thread.
 */
#ifdef __EMSCRIPTEN__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <strings.h>
#include <pthread.h>
#include <emscripten.h>
#include <emscripten/wasmfs.h>
#include <emscripten/threading.h>
#include "web.h"

#define FETCH_CHUNK (1u << 20)

EM_JS(char *, web_base_url, (void), {
    // The worker's script lives beside index.html: the site root.
    var u = new URL('./', self.location.href);
    return stringToNewUTF8(u.href);
});

EM_JS(void, web_status, (const char *text), {
    // stderr reaches the page (Module.printErr), which shows "[web]" lines as the status
    err('[web] ' + UTF8ToString(text));
});

void web_set_status(const char *text) { web_status(text); }

/* The fonts a Windows face is drawn with (gdidc.c looks here). */
static const char *const k_fonts[] = {
    "LiberationSans-Regular.ttf", "LiberationSans-Bold.ttf", "LiberationSans-Italic.ttf", "LiberationSans-BoldItalic.ttf",
    "LiberationSerif-Regular.ttf", "LiberationSerif-Bold.ttf", "LiberationSerif-Italic.ttf", "LiberationSerif-BoldItalic.ttf",
    "LiberationMono-Regular.ttf", "LiberationMono-Bold.ttf", "LiberationMono-Italic.ttf", "LiberationMono-BoldItalic.ttf" };

/* name, matched without case, in dir: 1 if there */
static int has_file(const char *dir, const char *name) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) found = strcasecmp(e->d_name, name) == 0;
    closedir(d);
    return found;
}

int web_mount(const char *exe_name, char *game_dir, size_t gn, char *save_dir, size_t sn) {
    char *base = web_base_url();
    if (!base) return 0;
    char url[1024];

    /* the fonts, from beside the page */
    snprintf(url, sizeof url, "%sfonts", base);
    backend_t fb = wasmfs_create_fetch_backend(url, FETCH_CHUNK);
    if (fb) {
        mkdir("/usr", 0777); mkdir("/usr/share", 0777); mkdir("/usr/share/fonts", 0777); mkdir("/usr/share/fonts/truetype", 0777);
        if (wasmfs_create_directory("/usr/share/fonts/truetype/liberation", 0777, fb) == 0)
            for (size_t i = 0; i < sizeof k_fonts / sizeof *k_fonts; i++) {
                char path[256];
                snprintf(path, sizeof path, "/usr/share/fonts/truetype/liberation/%s", k_fonts[i]);
                wasmfs_create_file(path, 0666, fb);
            }
    }

    /* the browser's own storage: the imported game folder (game/) and what
     * the game writes (save/); the page imports the folder (index.html) */
    backend_t opfs = wasmfs_create_opfs_backend();
    int have_opfs = opfs && wasmfs_create_directory("/opfs", 0777, opfs) == 0;
    if (have_opfs) {
        mkdir("/opfs/save", 0777);
        mkdir("/opfs/game", 0777);
        snprintf(save_dir, sn, "/opfs/save");
    } else {
        fprintf(stderr, "[web] no OPFS: saves and settings will not persist\n");
        mkdir("/save", 0777);
        snprintf(save_dir, sn, "/save");
    }
    free(base);
    if (!have_opfs || !has_file("/opfs/game", exe_name)) {
        fprintf(stderr, "[web] no game folder in this browser: import yours on the page\n");
        web_set_status("No game folder: import yours first.");
        return 0;
    }
    snprintf(game_dir, gn, "/opfs/game");
    web_set_status("Starting...");
    return 1;
}

/* WASMFS cannot name a directory that is a backend's root (its parent's
 * getName finds nothing), so getcwd() inside /game or /save answered "/".
 * The host absolutises relative paths with getcwd, so chdir and getcwd are
 * kept here instead: chdir does the real one and remembers the path. */
#include <errno.h>
#include <limits.h>
int __syscall_chdir(const char *path);
static char g_cwd[PATH_MAX] = "/";
static pthread_mutex_t g_cwd_lock = PTHREAD_MUTEX_INITIALIZER;
int chdir(const char *path) {
    int r = __syscall_chdir(path);
    if (r < 0) { errno = -r; return -1; }
    char joined[PATH_MAX * 2], out[PATH_MAX] = "/";
    pthread_mutex_lock(&g_cwd_lock);
    if (path[0] == '/') snprintf(joined, sizeof joined, "%s", path);
    else snprintf(joined, sizeof joined, "%s/%s", g_cwd, path);
    size_t o = 1;
    for (const char *p = joined; *p;) {                    /* normalise: no ., .., //, trailing / */
        while (*p == '/') p++;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t len = (size_t)(e - p);
        if (len == 2 && p[0] == '.' && p[1] == '.') { while (o > 1 && out[o - 1] != '/') o--; if (o > 1) o--; }
        else if (len && !(len == 1 && p[0] == '.') && o + len + 1 < sizeof out) {
            if (o > 1) out[o++] = '/';
            memcpy(out + o, p, len); o += len;
        }
        p = e;
    }
    out[o] = 0;
    snprintf(g_cwd, sizeof g_cwd, "%s", out);
    pthread_mutex_unlock(&g_cwd_lock);
    return 0;
}
char *getcwd(char *buf, size_t n) {
    pthread_mutex_lock(&g_cwd_lock);
    size_t l = strlen(g_cwd);
    if (!buf) { buf = malloc(n ? n : l + 1); n = n ? n : l + 1; }
    if (!buf || l >= n) { pthread_mutex_unlock(&g_cwd_lock); errno = buf ? ERANGE : ENOMEM; return NULL; }
    memcpy(buf, g_cwd, l + 1);
    pthread_mutex_unlock(&g_cwd_lock);
    return buf;
}

/* The pump's chance to run what the browser's main thread queued for this
 * one (input events SDL registered from here). */
void web_pump(void) { emscripten_current_thread_process_queued_calls(); }
#endif
