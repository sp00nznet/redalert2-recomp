/*
 * mods.c - play a mod from a folder (Windows host).
 *
 * A mod is a folder of the game's own kind of files (rules, art, a string
 * table, MIX files, maps) in mods\yr\<name>\ or mods\ra2\<name>\, beside the
 * build directory. The game folder is never changed: the file calls the game
 * makes are answered from the mod's folder where it has the file and from the
 * game's where it does not, directory listings show both, and what the game
 * writes while a mod is on (settings, saves) goes into the mod's folder. The
 * native Linux host does the same through win32hle's overlay (src/linux/mods.c).
 *
 * The mod is --mod NAME, else the one last chosen (ra2.ini beside ra2.exe,
 * [mods] active). The settings menu (F10) lists the mods; choosing one
 * restarts the game with it, since a game reads its rules once, at start.
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "native32.h"
#include "recomp_target.h"
#include "mods.h"

#ifdef RA2_TARGET_GAME
#define MODS_GAME "ra2"
#else
#define MODS_GAME "yr"
#endif

static char g_root[MAX_PATH];          /* mods\<game>, beside the build directory */
static char g_game[MAX_PATH];          /* the game's folder */
static char g_mod[MAX_PATH];           /* the active mod's folder, or "" */
static char g_active[128];
static char g_ini[MAX_PATH];           /* ra2.ini beside ra2.exe */
static char g_start_dir[MAX_PATH];     /* where the program started: a restart starts there too */

const char* mods_active(void) { return g_active; }

/* ---- paths: the mod's copy of a game file --------------------------------- */

/* path relative to the game's folder, if it is in it. */
static int game_relative(const char* path, char* rel, size_t n) {
    char full[MAX_PATH];
    size_t gl = strlen(g_game);
    if (!path || !GetFullPathNameA(path, MAX_PATH, full, NULL)) return 0;
    if (_strnicmp(full, g_game, gl) || (full[gl] != '\\' && full[gl] != 0)) return 0;
    _snprintf(rel, n - 1, "%s", full[gl] ? full + gl + 1 : ""), rel[n - 1] = 0;
    return 1;
}

static int is_file(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The path to read: the mod's file if it has one (a directory stays the game's). */
static const char* read_path(const char* path, char* buf) {
    char rel[MAX_PATH];
    if (!g_mod[0] || !game_relative(path, rel, sizeof rel) || !rel[0]) return path;
    _snprintf(buf, MAX_PATH - 1, "%s\\%s", g_mod, rel), buf[MAX_PATH - 1] = 0;
    return is_file(buf) ? buf : path;
}

/* The path to write: the mod's copy, its directories made, and with keep the
 * game's file copied there first (a write into the middle keeps the rest). */
static const char* write_path(const char* path, char* buf, int keep) {
    char rel[MAX_PATH];
    if (!g_mod[0] || !game_relative(path, rel, sizeof rel) || !rel[0]) return path;
    _snprintf(buf, MAX_PATH - 1, "%s\\%s", g_mod, rel), buf[MAX_PATH - 1] = 0;
    for (char* s = buf + strlen(g_mod) + 1; (s = strchr(s, '\\')); s++) {
        *s = 0;
        CreateDirectoryA(buf, NULL);
        *s = '\\';
    }
    if (keep && !is_file(buf) && is_file(path)) CopyFileA(path, buf, TRUE);
    return buf;
}

/* ---- the file calls ------------------------------------------------------- */

#define ARG(n) MEM32(g_esp + 4 + 4 * (n))
#define STR(i) ((const char*)(uintptr_t)ARG(i))

void mods_CreateFileA(void) {         /* (name, access, share, sa, disposition, flags, template) */
    char buf[MAX_PATH];
    DWORD access = ARG(1), disp = ARG(4);
    int writes = (access & GENERIC_WRITE) || disp == CREATE_NEW || disp == CREATE_ALWAYS || disp == OPEN_ALWAYS;
    int keep = (access & GENERIC_WRITE) && (disp == OPEN_EXISTING || disp == OPEN_ALWAYS);
    const char* path = writes ? write_path(STR(0), buf, keep) : read_path(STR(0), buf);
    g_eax = (uint32_t)(uintptr_t)CreateFileA(path, access, ARG(2), (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(3),
                                             disp, ARG(5), (HANDLE)(uintptr_t)ARG(6));
    g_esp += 4 + 7 * 4;
}

void mods_GetFileAttributesA(void) {
    char buf[MAX_PATH];
    g_eax = GetFileAttributesA(read_path(STR(0), buf));
    g_esp += 4 + 1 * 4;
}

void mods_CreateDirectoryA(void) {
    char buf[MAX_PATH];
    g_eax = (uint32_t)CreateDirectoryA(write_path(STR(0), buf, 0), (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(1));
    g_esp += 4 + 2 * 4;
}

void mods_CopyFileA(void) {           /* (from, to, fail if it exists) */
    char a[MAX_PATH], b[MAX_PATH];
    g_eax = (uint32_t)CopyFileA(read_path(STR(0), a), write_path(STR(1), b, 0), (BOOL)ARG(2));
    g_esp += 4 + 3 * 4;
}

/* A listing of a game directory while a mod is on: the mod's entries, then the
 * game's that the mod does not replace, all read at FindFirstFileA and handed
 * out one by one. Its handle is a slot here, not the system's. */
#define MAX_LISTINGS 16
#define LISTING_HANDLE(i) ((HANDLE)(uintptr_t)(0x4D4F0000u + (i)))
static struct { WIN32_FIND_DATAA* e; int n, at, used; } g_list[MAX_LISTINGS];

static int listing_of(HANDLE h) {
    uintptr_t v = (uintptr_t)h - 0x4D4F0000u;
    return v < MAX_LISTINGS && g_list[v].used ? (int)v : -1;
}

static void add_entries(int slot, const char* pattern) {
    WIN32_FIND_DATAA d;
    HANDLE h = FindFirstFileA(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        int dup = 0;
        for (int i = 0; i < g_list[slot].n && !dup; i++) dup = !_stricmp(g_list[slot].e[i].cFileName, d.cFileName);
        if (dup) continue;
        if (g_list[slot].n % 64 == 0)
            g_list[slot].e = realloc(g_list[slot].e, sizeof(WIN32_FIND_DATAA) * (g_list[slot].n + 64));
        g_list[slot].e[g_list[slot].n++] = d;
    } while (FindNextFileA(h, &d));
    FindClose(h);
}

void mods_FindFirstFileA(void) {      /* (pattern, &data) */
    char rel[MAX_PATH], mod_pattern[MAX_PATH];
    const char* pattern = STR(0);
    WIN32_FIND_DATAA* out = (WIN32_FIND_DATAA*)(uintptr_t)ARG(1);
    int slot = -1;
    if (g_mod[0] && game_relative(pattern, rel, sizeof rel))
        for (int i = 0; i < MAX_LISTINGS && slot < 0; i++)
            if (!g_list[i].used) slot = i;
    if (slot < 0) {
        g_eax = (uint32_t)(uintptr_t)FindFirstFileA(pattern, out);
    } else {
        _snprintf(mod_pattern, MAX_PATH - 1, "%s\\%s", g_mod, rel), mod_pattern[MAX_PATH - 1] = 0;
        g_list[slot].used = 1, g_list[slot].n = g_list[slot].at = 0;
        add_entries(slot, mod_pattern);              /* the mod's first: it wins a name both have */
        add_entries(slot, pattern);
        if (!g_list[slot].n) {
            g_list[slot].used = 0;
            SetLastError(ERROR_FILE_NOT_FOUND);
            g_eax = (uint32_t)(uintptr_t)INVALID_HANDLE_VALUE;
        } else {
            *out = g_list[slot].e[g_list[slot].at++];
            g_eax = (uint32_t)(uintptr_t)LISTING_HANDLE(slot);
        }
    }
    g_esp += 4 + 2 * 4;
}

void mods_FindNextFileA(void) {
    HANDLE h = (HANDLE)(uintptr_t)ARG(0);
    int slot = listing_of(h);
    if (slot < 0) {
        g_eax = (uint32_t)FindNextFileA(h, (WIN32_FIND_DATAA*)(uintptr_t)ARG(1));
    } else if (g_list[slot].at < g_list[slot].n) {
        *(WIN32_FIND_DATAA*)(uintptr_t)ARG(1) = g_list[slot].e[g_list[slot].at++];
        g_eax = TRUE;
    } else {
        SetLastError(ERROR_NO_MORE_FILES);
        g_eax = FALSE;
    }
    g_esp += 4 + 2 * 4;
}

void mods_FindClose(void) {
    HANDLE h = (HANDLE)(uintptr_t)ARG(0);
    int slot = listing_of(h);
    if (slot < 0) {
        g_eax = (uint32_t)FindClose(h);
    } else {
        free(g_list[slot].e);
        g_list[slot].e = NULL, g_list[slot].used = 0;
        g_eax = TRUE;
    }
    g_esp += 4 + 1 * 4;
}

/* ---- which mod ------------------------------------------------------------ */

int mods_list(char names[][128], int max) {
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA d;
    int n = 0;
    _snprintf(pattern, MAX_PATH - 1, "%s\\*", g_root), pattern[MAX_PATH - 1] = 0;
    HANDLE h = FindFirstFileA(pattern, &d);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do
        if ((d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && d.cFileName[0] != '.' && n < max)
            _snprintf(names[n], 127, "%s", d.cFileName), names[n++][127] = 0;
    while (FindNextFileA(h, &d));
    FindClose(h);
    return n;                                  /* NTFS lists names sorted */
}

void mods_start(const char* game_dir, const char* requested) {
    char exe[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, g_start_dir);   /* the host moves into the game's folder after this */
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char* slash = strrchr(exe, '\\');
    if (slash) *slash = 0;                     /* the build directory; mods\ is beside it */
    _snprintf(g_root, MAX_PATH - 1, "%s\\..\\mods\\" MODS_GAME, exe);
    GetFullPathNameA(g_root, MAX_PATH, g_root, NULL);
    _snprintf(g_ini, MAX_PATH - 1, "%s\\ra2.ini", exe);
    GetFullPathNameA(game_dir, MAX_PATH, g_game, NULL);

    if (requested) _snprintf(g_active, sizeof g_active - 1, "%s", requested);
    else GetPrivateProfileStringA("mods", "active", "", g_active, sizeof g_active, g_ini);
    if (!_stricmp(g_active, "none")) g_active[0] = 0;
    if (g_active[0]) {
        _snprintf(g_mod, MAX_PATH - 1, "%s\\%s", g_root, g_active), g_mod[MAX_PATH - 1] = 0;
        DWORD a = GetFileAttributesA(g_mod);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            fprintf(stderr, "[mods] no mod \"%s\" in %s: playing the game as it shipped\n", g_active, g_root);
            g_active[0] = g_mod[0] = 0;
        } else {
            fprintf(stderr, "[mods] playing %s (%s over the game's folder)\n", g_active, g_mod);
        }
    }
}

/* Restart with name ("" for none), remembered for the next start: the same
 * command line without its own --mod. */
void mods_restart_with(const char* name) {
    char cmd[8192], exe[MAX_PATH];
    int argc;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    WritePrivateProfileStringA("mods", "active", name, g_ini);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    _snprintf(cmd, sizeof cmd - 1, "\"%s\"", exe), cmd[sizeof cmd - 1] = 0;
    for (int i = 1; wargv && i < argc; i++) {
        char a[1024];
        if (!wcscmp(wargv[i], L"--mod") && i + 1 < argc) { i++; continue; }
        WideCharToMultiByte(CP_ACP, 0, wargv[i], -1, a, sizeof a, NULL, NULL);
        strncat(cmd, strchr(a, ' ') ? " \"" : " ", sizeof cmd - strlen(cmd) - 1);
        strncat(cmd, a, sizeof cmd - strlen(cmd) - 1);
        if (strchr(a, ' ')) strncat(cmd, "\"", sizeof cmd - strlen(cmd) - 1);
    }
    strncat(cmd, " --mod \"", sizeof cmd - strlen(cmd) - 1);
    strncat(cmd, name[0] ? name : "none", sizeof cmd - strlen(cmd) - 1);
    strncat(cmd, "\"", sizeof cmd - strlen(cmd) - 1);
    LocalFree(wargv);

    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    fprintf(stderr, "[mods] restarting with %s\n", name[0] ? name : "the game as it shipped");
    /* in the directory it first started in: relative arguments are relative to it */
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, g_start_dir, &si, &pi)) {
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
    fprintf(stderr, "[mods] could not restart (%lu): %s\n", GetLastError(), cmd);
}
