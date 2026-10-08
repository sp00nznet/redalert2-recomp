/* HD voxels: vehicles drawn at twice the resolution (docs/voxels.md).
 *
 * How the game draws a voxel unit (gamemd.exe 1.001):
 *   1. each part (body, turret, barrel) is rendered into a 256x256 buffer of
 *      palette indices (0x00B2FF78), and blitted through a remap into the
 *      256x256 staging surface ([0x00B1D13C]) by 0x004AF2A0;
 *   2. the finished staging image is copied onto the battlefield, with the
 *      unit's palette, lighting and the Z-buffer, by 0x004373B0 (call at
 *      0x0073B446);
 *   3. at the end of the frame the battlefield reaches the frame surface
 *      ([0x00887308], whose DirectDraw surface is the primary).
 *
 * What this adds, through run_lift.py HD_VOXEL_PATCHES:
 *   1. the render's finish stage runs three more times with every span
 *      starting half a pixel further left, up, or both; the four images
 *      interleave into one at 2x (ra2_vox_hd);
 *   2. each part's blit into staging is watched: the remap it applied and the
 *      pixels it wrote are learnt from the staging before and after, and the
 *      part's 2x image, remapped, is kept as a "stamp";
 *   3. the copy onto the battlefield is watched the same way: from the
 *      battlefield before and after it learns each index's final colour and
 *      which pixels the unit really got (the Z-buffer's say), and it records
 *      them with the 2x colours of the stamps under them;
 *   4. at the frame copy the records are published; the presenter's 2x frame
 *      (hdvox_compose) takes a record's four 2x pixels wherever the finished
 *      1x frame still shows exactly what the unit wrote there, so anything
 *      drawn over the unit afterwards keeps its 1x pixels.
 * Nothing the game sees changes: its buffers, rects and surfaces are as they
 * would have been. Off (the default), every hook returns at once.
 */
#ifdef _WIN32
#include <windows.h>
#include <ddraw.h>
#else
/* The native Linux host (src/linux): the few Windows types this file uses,
 * on POSIX, and the game's surfaces read through win32hle's DirectDraw. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sys/stat.h>
#include <time.h>
#include "win32hle.h"
typedef union { long long QuadPart; } LARGE_INTEGER;
static void QueryPerformanceCounter(LARGE_INTEGER* t) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->QuadPart = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}
static void QueryPerformanceFrequency(LARGE_INTEGER* t) { t->QuadPart = 1000000000LL; }
typedef pthread_mutex_t CRITICAL_SECTION;
#define InitializeCriticalSection(c) pthread_mutex_init((c), NULL)
#define EnterCriticalSection(c) pthread_mutex_lock(c)
#define LeaveCriticalSection(c) pthread_mutex_unlock(c)
#define CreateDirectoryA(p, sa) mkdir((p), 0755)
#define VirtualAlloc(a, n, t, p) calloc(1, (n))
typedef int32_t LONG;
#define InterlockedIncrement(p) __sync_add_and_fetch((p), 1)
#define MAX_PATH 4096
#define _snprintf snprintf
#define BI_RGB 0
typedef struct { uint32_t biSize; int32_t biWidth, biHeight; uint16_t biPlanes, biBitCount;
                 uint32_t biCompression, biSizeImage; int32_t biXPelsPerMeter, biYPelsPerMeter;
                 uint32_t biClrUsed, biClrImportant; } BITMAPINFOHEADER;
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hdvox.h"
#include "recomp_target.h"

#ifdef RA2_TARGET_GAMEMD
#define VOX_COLOUR ((uint8_t*)(uintptr_t)0x00B2FF78u)   /* 256x256 palette indices */
#define VOX_DEPTH  ((uint8_t*)(uintptr_t)0x00B1D5E0u)   /* 256x256, with the Z flag */
#define VOX_BBOX   ((uint32_t*)(uintptr_t)0x00B2FB60u)  /* x, y, w, h, list count */
#define VOX_PAL    ((const uint8_t*)(uintptr_t)0x00B2FB78u)
#define STAGING    (*(const uint32_t*)(uintptr_t)0x00B1D13Cu)
#define FRAME_SURF (*(const uint32_t*)(uintptr_t)0x00887308u)
#define VOX_DIRTY  0x00B1CFC0u                           /* the staging's dirty rect */
#define VOX_SHADOW 0x00B2D928u                           /* the shadow mask buffer */
#else                                                    /* Red Alert 2's, by shape */
#define VOX_COLOUR ((uint8_t*)(uintptr_t)0x00AE3408u)
#define VOX_DEPTH  ((uint8_t*)(uintptr_t)0x00AD0A70u)
#define VOX_BBOX   ((uint32_t*)(uintptr_t)0x00AE2FF0u)
#define VOX_PAL    ((const uint8_t*)(uintptr_t)0x00AE3008u)
#define STAGING    (*(const uint32_t*)(uintptr_t)0x00AD0604u)
#define FRAME_SURF (*(const uint32_t*)(uintptr_t)0x00839950u)
#define VOX_DIRTY  0x00AD0490u
#define VOX_SHADOW 0x00AE0DB8u
#endif

int ra2_vox_hd_on;
int16_t ra2_vox_dx, ra2_vox_dy;          /* read by the rasterizer patch, 8.8 */

static int g_busy;
static LARGE_INTEGER g_t0;
static long long g_hd_ticks, g_frame_ticks, g_last_frame;   /* QueryPerformanceCounter */
static const char* g_dump;
static int g_dumped;
static uint8_t g_pass[4][65536];          /* the four passes */
static uint8_t g_depth[65536];
static uint32_t g_bbox[5], g_rect[16], *g_rect_at, g_rect_len;   /* the caller's results, put back */
uint8_t ra2_vox_hd[512 * 512];            /* the last part rendered, at 2x */

/* pass k's offset: half a pixel back, so its pixels sit at +1 in the 2x grid */
static const int16_t k_off[4][2] = { { 0, 0 }, { -0x80, 0 }, { 0, -0x80 }, { -0x80, -0x80 } };

void hdvox_configure(int on, const char* dump_dir) {
    ra2_vox_hd_on = on || dump_dir;
    g_dump = dump_dir;
    if (dump_dir) CreateDirectoryA(dump_dir, NULL);
}

/* ---- 1. the render at 2x ---------------------------------------------------- */

static void write_bmp(const char* path, const uint8_t* px, int w, int h) {
    FILE* f = fopen(path, "wb");
    BITMAPINFOHEADER ih = { sizeof ih, w, h, 1, 8, BI_RGB };
    uint32_t off = 14 + sizeof ih + 1024, size = off + w * h, zero = 0;
    int shift = 2;                                       /* VGA 6-bit, unless it is 8-bit */
    if (!f) return;
    for (int i = 0; i < 768; i++) if (VOX_PAL[i] > 63) shift = 0;
    fwrite("BM", 1, 2, f);
    fwrite(&size, 4, 1, f), fwrite(&zero, 4, 1, f), fwrite(&off, 4, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    for (int i = 0; i < 256; i++) {
        uint8_t q[4] = { (uint8_t)(VOX_PAL[i * 3 + 2] << shift), (uint8_t)(VOX_PAL[i * 3 + 1] << shift),
                         (uint8_t)(VOX_PAL[i * 3] << shift), 0 };
        if (i == 0) q[0] = 0x40, q[1] = 0x40, q[2] = 0x40;   /* transparent shows grey */
        fwrite(q, 4, 1, f);
    }
    for (int y = h - 1; y >= 0; y--) fwrite(px + y * w, 1, w, f);   /* bottom-up */
    fclose(f);
}

/* The 2x image of a render, remembered by its 1x pixels: a unit standing
 * still, or turned to a facing seen before, renders the same 1x image, and
 * its 2x image is taken from here instead of three more passes. The key is
 * a hash of the 1x pixels inside the render's rect (source x, y at rect[2],
 * rect[3]; size at rect[4], rect[5]); the value is that rect at 2x. */
#define MEMO 1024
typedef struct { uint64_t key; int w, h; uint8_t* px; } memo_t;
static memo_t g_memo[MEMO];
static int g_memo_next;
static long g_memo_hits, g_memo_misses;
static uint64_t g_key;
static int g_mx, g_my, g_mw, g_mh;

static int memo_rect(const uint32_t* r) {
    g_mx = (int)r[2], g_my = (int)r[3], g_mw = (int)r[4], g_mh = (int)r[5];
    return g_mx >= 0 && g_my >= 0 && g_mw > 0 && g_mh > 0 && g_mx + g_mw <= 256 && g_my + g_mh <= 256;
}

static uint64_t memo_key(void) {
    uint64_t k = 1469598103934665603ull ^ ((uint64_t)g_mw << 32 | (uint64_t)g_mh);
    for (int y = 0; y < g_mh; y++) {
        const uint8_t* row = VOX_COLOUR + (g_my + y) * 256 + g_mx;
        for (int x = 0; x < g_mw; x++) k = (k ^ row[x]) * 1099511628211ull;
    }
    return k;
}

static int hd_begin(uint32_t save, uint32_t len, int memo_ok);

/* After 0x00754510: rect is the 6-dword rect it returned. */
int ra2_vox_hd_begin(uint32_t rect) {
    return hd_begin(rect, 24, memo_rect((const uint32_t*)(uintptr_t)rect));
}

/* After 0x007542F0, which writes its results through pointers: save is the
 * caller's region holding them; the memo's rect is the buffer's bounding
 * box (0x00B2FB60: x, y, w, h, inclusive), which every finish stage sets. */
static long g_anim_renders;

/* Voxel animations and debris are opt-in, RA2_HD_VOXEL_ANIMS=1: the path is
 * the same as units' and shadows', but no test yet puts one on screen. */
static int anims_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("RA2_HD_VOXEL_ANIMS") != NULL;
    return on && ra2_vox_hd_on;
}

int ra2_vox_hd_begin_at(uint32_t save, uint32_t len) {
    if (!anims_on()) return 0;
    g_anim_renders++;
    g_mx = (int)VOX_BBOX[0], g_my = (int)VOX_BBOX[1], g_mw = (int)VOX_BBOX[2] + 1, g_mh = (int)VOX_BBOX[3] + 1;
    int ok = g_mx >= 0 && g_my >= 0 && g_mw > 0 && g_mh > 0 && g_mx + g_mw <= 256 && g_my + g_mh <= 256;
    return hd_begin(save, len, ok);
}

static int hd_begin(uint32_t save, uint32_t len, int memo_ok) {
    if (!ra2_vox_hd_on || g_busy || len > sizeof g_rect) return 0;
    g_key = 0;
    if (memo_ok) {
        g_key = memo_key();
        for (int i = 0; i < MEMO; i++)
            if (g_memo[i].px && g_memo[i].key == g_key && g_memo[i].w == g_mw && g_memo[i].h == g_mh) {
                for (int y = 0; y < 2 * g_mh; y++)
                    memcpy(ra2_vox_hd + (2 * g_my + y) * 512 + 2 * g_mx, g_memo[i].px + y * 2 * g_mw, 2 * g_mw);
                g_memo_hits++;
                return 0;                         /* no passes: the game goes on with its 1x */
            }
    }
    g_memo_misses++;
    g_busy = 1;
    QueryPerformanceCounter(&g_t0);
    memcpy(g_pass[0], VOX_COLOUR, 65536);
    memcpy(g_depth, VOX_DEPTH, 65536);
    memcpy(g_bbox, VOX_BBOX, sizeof g_bbox);
    g_rect_at = (uint32_t*)(uintptr_t)save;
    g_rect_len = len;
    memcpy(g_rect, g_rect_at, len);
    return 1;
}

void ra2_vox_hd_pass(int k) {
    if (k > 1) memcpy(g_pass[k - 1], VOX_COLOUR, 65536);
    memset(VOX_COLOUR, 0, 65536);                 /* as 0x00753E00 clears them */
    memset(VOX_DEPTH, 0, 65536);
    ra2_vox_dx = k_off[k][0], ra2_vox_dy = k_off[k][1];
}

uint32_t ra2_vox_hd_end(void) {
    memcpy(g_pass[3], VOX_COLOUR, 65536);
    ra2_vox_dx = ra2_vox_dy = 0;
    memcpy(VOX_COLOUR, g_pass[0], 65536);         /* the game's own 1x render, as it was */
    memcpy(VOX_DEPTH, g_depth, 65536);
    memcpy(VOX_BBOX, g_bbox, sizeof g_bbox);
    memcpy(g_rect_at, g_rect, g_rect_len);
    for (int k = 0; k < 4; k++) {
        int dx = k & 1, dy = k >> 1;
        for (int y = 0; y < 256; y++)
            for (int x = 0; x < 256; x++)
                ra2_vox_hd[(2 * y + dy) * 512 + 2 * x + dx] = g_pass[k][y * 256 + x];
    }
    if (g_key) {                                  /* remember it */
        memo_t* m = &g_memo[g_memo_next];
        g_memo_next = (g_memo_next + 1) % MEMO;
        free(m->px);
        m->px = (uint8_t*)malloc((size_t)4 * g_mw * g_mh);
        if (m->px) {
            m->key = g_key, m->w = g_mw, m->h = g_mh;
            for (int y = 0; y < 2 * g_mh; y++)
                memcpy(m->px + y * 2 * g_mw, ra2_vox_hd + (2 * g_my + y) * 512 + 2 * g_mx, 2 * g_mw);
        }
    }
    if (g_dump && g_dumped < 40) {
        char path[MAX_PATH];
        _snprintf(path, sizeof path - 1, "%s/vox_%02d_1x.bmp", g_dump, g_dumped), path[sizeof path - 1] = 0;
        write_bmp(path, g_pass[0], 256, 256);
        _snprintf(path, sizeof path - 1, "%s/vox_%02d_2x.bmp", g_dump, g_dumped), path[sizeof path - 1] = 0;
        write_bmp(path, ra2_vox_hd, 512, 512);
        g_dumped++;
    }
    g_busy = 0;
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        g_hd_ticks += t.QuadPart - g_t0.QuadPart;
    }
    return (uint32_t)(uintptr_t)g_rect_at;
}

/* ---- 2. parts into staging ---------------------------------------------------- */

typedef struct {
    int x, y, w, h;                       /* on staging */
    uint8_t before[65536], after[65536];  /* w*h each */
    uint8_t hd[4 * 65536];                /* (2w)x(2h), remapped; 0 transparent */
} stamp_t;

#define MAX_STAMPS 8
static stamp_t g_stamps[MAX_STAMPS];
static int g_nstamps, g_stamp_open;
static int g_src_x, g_src_y;              /* the part's rect in the voxel buffer */

static uint8_t* staging_px(void) { return (uint8_t*)(uintptr_t)((const uint32_t*)(uintptr_t)STAGING)[5]; }

static int g_direct_open, g_dir_x, g_dir_y, g_dir_w, g_dir_h, g_dir_sx, g_dir_sy;
static uint32_t g_dir_dest;
static long g_direct;
static void direct_blit(uint32_t dest, const int32_t* r, const int32_t* pt);
static int dsurf_read(uint32_t ds, int x, int y, int w, int h, uint16_t* out);
static void record_image(int x, int y, int w, int h, const uint8_t* idx1, int s1, const uint8_t* idx2, int s2);
static uint16_t g_before[480 * 1024], g_after[480 * 1024];

/* 0x00707233, before 0x004AF2A0: ecx the destination, then on the stack the
 * source surface, its rect (x, y, w, h) and the destination point. */
void ra2_vox_hd_blit(uint32_t dest, uint32_t convert, uint32_t esp) {
    const uint32_t* a = (const uint32_t*)(uintptr_t)esp;
    const int32_t* r = (const int32_t*)(uintptr_t)a[1];
    const int32_t* pt = (const int32_t*)(uintptr_t)a[2];
    (void)convert;
    g_stamp_open = 0;
    g_direct_open = 0;
    if (!ra2_vox_hd_on) return;
    if (dest != STAGING) {                        /* aircraft: straight onto the battlefield */
        direct_blit(dest, r, pt);
        return;
    }
    if (!staging_px()) return;
    if (g_nstamps == MAX_STAMPS) {                /* a unit never copied out: drop the oldest */
        memmove(&g_stamps[0], &g_stamps[1], sizeof g_stamps[0] * (MAX_STAMPS - 1));
        g_nstamps--;
    }
    stamp_t* s = &g_stamps[g_nstamps];
    s->x = pt[0], s->y = pt[1], s->w = r[2], s->h = r[3];
    g_src_x = r[0], g_src_y = r[1];
    if (s->x < 0 || s->y < 0 || s->w <= 0 || s->h <= 0 || s->x + s->w > 256 || s->y + s->h > 256 ||
        g_src_x < 0 || g_src_y < 0 || g_src_x + s->w > 256 || g_src_y + s->h > 256)
        return;                                   /* clipped: leave the part at 1x */
    const uint8_t* st = staging_px();
    for (int j = 0; j < s->h; j++) memcpy(s->before + j * s->w, st + (s->y + j) * 256 + s->x, s->w);
    g_stamp_open = 1;
}

void ra2_vox_hd_blitted(void) {
    if (g_direct_open) {
        g_direct_open = 0;
        if (dsurf_read(g_dir_dest, g_dir_x, g_dir_y, g_dir_w, g_dir_h, g_after))
            record_image(g_dir_x, g_dir_y, g_dir_w, g_dir_h, VOX_COLOUR + g_dir_sy * 256 + g_dir_sx, 256,
                         ra2_vox_hd + 2 * g_dir_sy * 512 + 2 * g_dir_sx, 512);
        g_direct++;
        return;
    }
    if (!g_stamp_open) return;
    g_stamp_open = 0;
    stamp_t* s = &g_stamps[g_nstamps];
    const uint8_t* st = staging_px();
    uint8_t remap[256], known[256] = { 0 };
    for (int j = 0; j < s->h; j++) memcpy(s->after + j * s->w, st + (s->y + j) * 256 + s->x, s->w);
    /* the remap the blit applied, learnt from what it wrote */
    for (int j = 0; j < s->h; j++)
        for (int i = 0; i < s->w; i++) {
            uint8_t v = VOX_COLOUR[(g_src_y + j) * 256 + g_src_x + i];
            if (v && !known[v]) remap[v] = s->after[j * s->w + i], known[v] = 1;
        }
    int w2 = 2 * s->w;
    for (int j = 0; j < 2 * s->h; j++)
        for (int i = 0; i < w2; i++) {
            uint8_t v = ra2_vox_hd[(2 * g_src_y + j) * 512 + 2 * g_src_x + i];
            /* an index the 1x part never used: its 1x pixel's colour */
            s->hd[j * w2 + i] = !v ? 0 : known[v] ? remap[v] : s->after[(j / 2) * s->w + i / 2];
        }
    g_nstamps++;
}

/* ---- 3. the unit onto the battlefield ----------------------------------------------- */

typedef struct { int16_t x, y, w, h, kind, pad; } rec_t;   /* kind 0 unit, 1 shadow; then E[w*h], O[4*w*h] (uint16), m[w*h] */

#define ARENA (24u << 20)
static uint8_t* g_arena[2];
static size_t g_used[2];
static int g_build;                               /* the arena being built this frame */
static CRITICAL_SECTION g_pub_lock;
static int g_pub_lock_init;
static long g_records, g_published_frames;

static size_t g_need;

/* A record of w x h battlefield pixels at (x, y): E what the frame should
 * show at 1x, O the four 2x pixels, m which pixels count. NULL when full. */
static rec_t* rec_new(int x, int y, int w, int h, uint16_t** E, uint16_t** O, uint8_t** m) {
    g_need = sizeof(rec_t) + (size_t)w * h * (2 + 8 + 1);
    if (!g_arena[0]) {
        g_arena[0] = (uint8_t*)VirtualAlloc(NULL, ARENA, MEM_COMMIT, PAGE_READWRITE);
        g_arena[1] = (uint8_t*)VirtualAlloc(NULL, ARENA, MEM_COMMIT, PAGE_READWRITE);
        if (!g_arena[0] || !g_arena[1]) { ra2_vox_hd_on = 0; return NULL; }
    }
    if (g_used[g_build] + g_need > ARENA) return NULL;
    rec_t* r = (rec_t*)(g_arena[g_build] + g_used[g_build]);
    *E = (uint16_t*)(r + 1);
    *O = *E + w * h;
    *m = (uint8_t*)(*O + 4 * w * h);
    r->x = (int16_t)x, r->y = (int16_t)y, r->w = (int16_t)w, r->h = (int16_t)h, r->kind = 0, r->pad = 0;
    return r;
}

static void rec_commit(void) {
    g_used[g_build] += (g_need + 7) & ~(size_t)7;
    g_records++;
}

/* g_before, g_after: the battlefield around a draw (declared above) */

/* An aircraft's part, blitted by 0x004AF2A0 straight onto the 16-bit
 * battlefield: the battlefield around it before (and after, in
 * ra2_vox_hd_blitted) the blit. */
static void direct_blit(uint32_t dest, const int32_t* r, const int32_t* pt) {
    const uint32_t* ds = (const uint32_t*)(uintptr_t)dest;
    if (ds[4] != 2) return;
    int x = pt[0], y = pt[1], w = r[2], h = r[3], sx = r[0], sy = r[1];
    if (x < 0) sx -= x, w += x, x = 0;
    if (y < 0) sy -= y, h += y, y = 0;
    if (x + w > (int)ds[1]) w = (int)ds[1] - x;
    if (y + h > (int)ds[2]) h = (int)ds[2] - y;
    if (w <= 0 || h <= 0 || w > 1024 || h > 480 || sx < 0 || sy < 0 || sx + w > 256 || sy + h > 256) return;
    if (!dsurf_read(dest, x, y, w, h, g_before)) return;
    g_dir_dest = dest, g_dir_x = x, g_dir_y = y, g_dir_w = w, g_dir_h = h, g_dir_sx = sx, g_dir_sy = sy;
    g_direct_open = 1;
}

/* A record for an image drawn onto the battlefield at (x, y), w x h: idx1
 * its 1x palette indices (stride s1), idx2 the same at 2x (stride s2), and
 * g_before/g_after the battlefield around the draw. Each index's colour, and
 * which pixels the image got (the Z-buffer's say), come from what changed. */
static void record_image(int x, int y, int w, int h, const uint8_t* idx1, int s1, const uint8_t* idx2, int s2) {
    uint16_t col[256];
    uint8_t known[256] = { 0 };
    int got = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            uint8_t v = idx1[j * s1 + i];
            if (v && g_after[j * w + i] != g_before[j * w + i]) {
                got++;
                if (!known[v]) col[v] = g_after[j * w + i], known[v] = 1;
            }
        }
    if (!got) return;
    uint16_t *E, *O;
    uint8_t* m;
    if (!rec_new(x, y, w, h, &E, &O, &m)) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int p = j * w + i;
            uint16_t b = g_before[p], f = g_after[p];
            m[p] = f != b && idx1[j * s1 + i];
            E[p] = f;
            if (!m[p]) continue;
            for (int q = 0; q < 4; q++) {
                uint8_t v = idx2[(2 * j + (q >> 1)) * s2 + 2 * i + (q & 1)];
                /* transparent at 2x: what was there before; an index the 1x
                 * image never used: the 1x pixel */
                O[p * 4 + q] = !v ? b : known[v] ? col[v] : f;
            }
        }
    rec_commit();
}   /* the unit's rect on the battlefield */
static uint32_t g_copy_dest;
static int g_cx, g_cy, g_cw, g_ch, g_sx, g_sy, g_copy_open;

#ifndef _WIN32
/* Copy w x h 16-bit pixels at (x, y) of a game DSurface out: its DirectDraw
 * surface (at +0x1C) is win32hle's, whose pixels are read directly, locked by
 * the game or not. */
static int dsurf_read(uint32_t ds, int x, int y, int w, int h, uint16_t* out) {
    uint8_t* px;
    int sw, sh, pitch, bpp;
    if (!hle_dd_surface_pixels(((const uint32_t*)(uintptr_t)ds)[7], &px, &sw, &sh, &pitch, &bpp) || bpp != 16) return 0;
    if (x < 0 || y < 0 || x + w > sw || y + h > sh) return 0;
    for (int j = 0; j < h; j++) memcpy(out + j * w, px + (size_t)(y + j) * pitch + (size_t)x * 2, (size_t)w * 2);
    return 1;
}
#else
static IDirectDrawSurface* dsurf_dd(uint32_t ds) { return (IDirectDrawSurface*)(uintptr_t)((const uint32_t*)(uintptr_t)ds)[7]; }

/* Copy w x h 16-bit pixels at (x, y) of a game DSurface out (dir 0) or back. */
static int dsurf_read(uint32_t ds, int x, int y, int w, int h, uint16_t* out) {
    IDirectDrawSurface* dd = dsurf_dd(ds);
    const uint32_t* f = (const uint32_t*)(uintptr_t)ds;
    DDSURFACEDESC d;
    /* Locked by the game (count at +0x0C, pixels at +0x14): read through its
     * pointer, with the pitch DirectDraw reports for the surface. */
    if (f[3]) {
        memset(&d, 0, sizeof d);
        d.dwSize = sizeof d;
        if (!dd || !f[5] || f[4] != 2 || dd->lpVtbl->GetSurfaceDesc(dd, &d) != DD_OK) return 0;
        if (d.lPitch < (x + w) * 2 || (DWORD)(y + h) > d.dwHeight) return 0;
        for (int j = 0; j < h; j++)
            memcpy(out + j * w, (const uint8_t*)(uintptr_t)f[5] + (y + j) * d.lPitch + x * 2, (size_t)w * 2);
        return 1;
    }
    if (!dd) return 0;
    memset(&d, 0, sizeof d);
    d.dwSize = sizeof d;
    HRESULT hr = dd->lpVtbl->Lock(dd, NULL, &d, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (hr != DD_OK) return 0;
    int ok = d.ddpfPixelFormat.dwRGBBitCount == 16;
    if (ok)
        for (int j = 0; j < h; j++)
            memcpy(out + j * w, (const uint8_t*)d.lpSurface + (y + j) * d.lPitch + x * 2, (size_t)w * 2);
    dd->lpVtbl->Unlock(dd, NULL);
    return ok;
}
#endif

/* 0x0073B43F, before 0x004373B0: ecx the battlefield surface; on the stack the
 * destination rect, the staging surface, the (clipped) source rect. */
void ra2_vox_unit_copy(uint32_t dest, uint32_t esp) {
    const uint32_t* a = (const uint32_t*)(uintptr_t)esp;
    const int32_t* dr = (const int32_t*)(uintptr_t)a[0];
    const int32_t* sr = (const int32_t*)(uintptr_t)a[2];
    const uint32_t* ds = (const uint32_t*)(uintptr_t)dest;
    g_copy_open = 0;
    if (!ra2_vox_hd_on || a[1] != STAGING) { g_nstamps = 0; return; }
    /* The source is the staging's dirty rect, the parts' union (0x00B1CFC0);
     * the stack's third rect is only the staging's bounds. Clip the
     * destination to the surface, the source with it. */
    const int32_t* dirty = (const int32_t*)(uintptr_t)VOX_DIRTY;
    int x = dr[0], y = dr[1], w = dr[2], h = dr[3], sx = dirty[0], sy = dirty[1];
    (void)sr;
    if (x < 0) sx -= x, w += x, x = 0;
    if (y < 0) sy -= y, h += y, y = 0;
    if (x + w > (int)ds[1]) w = (int)ds[1] - x;
    if (y + h > (int)ds[2]) h = (int)ds[2] - y;
    if (w <= 0 || h <= 0 || w > 1024 || h > 480 || sx < 0 || sy < 0 || sx + w > 256 || sy + h > 256) {
        g_nstamps = 0;
        return;
    }
    if (!dsurf_read(dest, x, y, w, h, g_before)) { g_nstamps = 0; return; }
    g_copy_dest = dest, g_cx = x, g_cy = y, g_cw = w, g_ch = h, g_sx = sx, g_sy = sy;
    g_copy_open = 1;
}

void ra2_vox_unit_copied(void) {
    static uint8_t s2[512 * 512];                 /* staging at 2x, for this unit */
    if (!g_copy_open) return;
    g_copy_open = 0;
    int w = g_cw, h = g_ch, sx = g_sx, sy = g_sy;
    const uint8_t* st = staging_px();
    if (!st || !dsurf_read(g_copy_dest, g_cx, g_cy, w, h, g_after)) { g_nstamps = 0; return; }
    /* staging at 2x: each pixel four times, then the stamps' 2x pixels where
     * the stamp's own 1x pixel is still there (a later part did not cover it) */
    for (int j = 0; j < 2 * h; j++)
        for (int i = 0; i < 2 * w; i++)
            s2[j * 512 + i] = st[(sy + j / 2) * 256 + sx + i / 2];
    for (int k = 0; k < g_nstamps; k++) {
        const stamp_t* s = &g_stamps[k];
        for (int j = 0; j < s->h; j++)
            for (int i = 0; i < s->w; i++) {
                int px = s->x + i - sx, py = s->y + j - sy;      /* in this copy's 1x rect */
                if (px < 0 || py < 0 || px >= w || py >= h) continue;
                uint8_t now = st[(s->y + j) * 256 + s->x + i];
                uint8_t was = s->before[j * s->w + i], put = s->after[j * s->w + i];
                if (now != put) continue;                      /* covered later */
                int wrote = put != was;
                for (int q = 0; q < 4; q++) {
                    uint8_t v = s->hd[(2 * j + (q >> 1)) * 2 * s->w + 2 * i + (q & 1)];
                    if (v) s2[(2 * py + (q >> 1)) * 512 + 2 * px + (q & 1)] = v;
                    else if (wrote) s2[(2 * py + (q >> 1)) * 512 + 2 * px + (q & 1)] = was;
                }
            }
    }
    g_nstamps = 0;
    record_image(g_cx, g_cy, w, h, st + sy * 256 + sx, 256, s2, 512);
}

/* ---- 3b. shadows --------------------------------------------------------------------- */
/* 0x00707280 renders a unit's shadow as a 0/1 mask in the voxel buffer (at
 * 2x too, through the same passes) and blits it onto the battlefield with
 * the shadow converter, which darkens what is there. Around that blit the
 * host learns the darkening from the battlefield before and after, and
 * records the shadow's 2x edge: a 2x pixel in the shadow gets the darkened
 * colour, one outside it the colour from before. */

static int g_sh_open, g_sh_x, g_sh_y, g_sh_w, g_sh_h, g_sh_sx, g_sh_sy;
static uint32_t g_sh_dest;
static long g_sh_records, g_sh_skipped;

/* 0x00707431, before 0x004AF2A0: ecx the destination; on the stack the voxel
 * surface, the shadow's rect in it and the destination point. */
void ra2_vox_shadow_blit(uint32_t dest, uint32_t esp) {
    const uint32_t* a = (const uint32_t*)(uintptr_t)esp;
    const int32_t* r = (const int32_t*)(uintptr_t)a[1];
    const int32_t* pt = (const int32_t*)(uintptr_t)a[2];
    const uint32_t* ds = (const uint32_t*)(uintptr_t)dest;
    g_sh_open = 0;
    if (!ra2_vox_hd_on || a[0] != VOX_SHADOW) return;
    if (ds[4] != 2) { g_sh_skipped++; return; }  /* not the 16-bit battlefield */
    int x = pt[0], y = pt[1], w = r[2], h = r[3], sx = r[0], sy = r[1];
    if (x < 0) sx -= x, w += x, x = 0;
    if (y < 0) sy -= y, h += y, y = 0;
    if (x + w > (int)ds[1]) w = (int)ds[1] - x;
    if (y + h > (int)ds[2]) h = (int)ds[2] - y;
    if (w <= 0 || h <= 0 || w > 1024 || h > 480 || sx < 0 || sy < 0 || sx + w > 256 || sy + h > 256) return;
    if (!dsurf_read(dest, x, y, w, h, g_before)) return;
    g_sh_dest = dest, g_sh_x = x, g_sh_y = y, g_sh_w = w, g_sh_h = h, g_sh_sx = sx, g_sh_sy = sy;
    g_sh_open = 1;
}

static uint16_t half565(uint16_t c) { return (uint16_t)((c >> 1) & 0x7BEF); }

void ra2_vox_shadow_blitted(void) {
    if (!g_sh_open) return;
    g_sh_open = 0;
    int w = g_sh_w, h = g_sh_h, sx = g_sh_sx, sy = g_sh_sy;
    if (!dsurf_read(g_sh_dest, g_sh_x, g_sh_y, w, h, g_after)) return;
    /* the darkening: the classic 16-bit half, or failing that, whatever the
     * blit did to each colour it touched */
    int pairs = 0, halves = 0;
    for (int k = 0; k < w * h; k++)
        if (g_after[k] != g_before[k]) pairs++, halves += g_after[k] == half565(g_before[k]);
    if (!pairs) return;
    int is_half = halves == pairs;
    uint16_t *E, *O;
    uint8_t* m;
    /* RA2's voxel shadow is a stipple even at 1x (its plotter writes 0 or 1
     * per position, last write wins), so the four passes are four different
     * stipples, and interleaved they read as a checkerboard. At 2x the
     * shadow is their union with the holes closed (dilate, then erode, 3x3):
     * a solid shadow with a 2x edge. */
    static uint8_t u[2 * 256 * 2 * 256], t[2 * 256 * 2 * 256];
    int W = 2 * w, H = 2 * h;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            u[y * W + x] = ra2_vox_hd[(2 * sy + y) * 512 + 2 * sx + x] != 0;
    for (int pass = 0; pass < 2; pass++) {       /* 0 dilate u -> t, 1 erode t -> u */
        const uint8_t* src = pass ? t : u;
        uint8_t* dst = pass ? u : t;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int any = 0, all = 1;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        int yy = y + dy, xx = x + dx;
                        int v = yy >= 0 && yy < H && xx >= 0 && xx < W && src[yy * W + xx];
                        any |= v, all &= v;
                    }
                dst[y * W + x] = (uint8_t)(pass ? all : any);
            }
    }
    rec_t* rr = rec_new(g_sh_x, g_sh_y, w, h, &E, &O, &m);
    if (!rr) return;
    rr->kind = 1;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int p = j * w + i;
            uint16_t b = g_before[p], f = g_after[p];
            int dark1 = f != b;
            E[p] = f;
            m[p] = 0;
            for (int q = 0; q < 4; q++) {
                int in = u[(2 * j + (q >> 1)) * W + 2 * i + (q & 1)];
                uint16_t v = !in ? b : dark1 ? f : is_half ? half565(b) : f;
                O[p * 4 + q] = v;
                m[p] |= v != f;                   /* only where 2x differs from 1x */
            }
        }
    rec_commit();
    g_sh_records++;
}

/* VoxelAnimClass::Draw_It's two blits: the shadow's and the body's (straight
 * onto the battlefield), recorded like a unit's when animations are on. */
void ra2_vox_anim_shadow_blit(uint32_t dest, uint32_t esp) { if (anims_on()) ra2_vox_shadow_blit(dest, esp); }
void ra2_vox_anim_shadow_blitted(void) { if (anims_on()) ra2_vox_shadow_blitted(); }
void ra2_vox_anim_blit(uint32_t dest, uint32_t convert, uint32_t esp) { if (anims_on()) ra2_vox_hd_blit(dest, convert, esp); }
void ra2_vox_anim_blitted(void) { if (anims_on()) ra2_vox_hd_blitted(); }

/* ---- 4. the frame ------------------------------------------------------------------ */

/* Copies into the frame surface: in a battle the game draws the screen this
 * way, not by blitting into the primary, so this is how the presenter knows a
 * new frame is there (host_frame_count). */
volatile LONG ra2_frame_copies;

/* 0x004373B0 entry: a copy into the frame surface ends the frame. */
void ra2_vox_frame_blit(uint32_t dest, uint32_t argp) {
    (void)argp;
    static int empty_copies, stats = -1;
    if (dest == FRAME_SURF) InterlockedIncrement(&ra2_frame_copies);
    if (stats < 0) stats = getenv("RA2_FRAME_STATS") != NULL;
    if (stats && dest == FRAME_SURF) {            /* frame time, HD voxels on or off */
        static long long last, sum;
        static long n;
        LARGE_INTEGER t, hz;
        QueryPerformanceCounter(&t);
        if (last) sum += t.QuadPart - last, n++;
        last = t.QuadPart;
        if (n == 2000) {
            QueryPerformanceFrequency(&hz);
            fprintf(stderr, "[frames] %.2f ms between frame copies (HD voxels %s)\n",
                    1000.0 * sum / hz.QuadPart / n, ra2_vox_hd_on ? "on" : "off");
            sum = 0, n = 0;
        }
    }
    if (!ra2_vox_hd_on || dest != FRAME_SURF) return;
    if (!g_arena[0]) return;
    /* The frame surface gets more than one copy a frame: publish when this
     * frame recorded something, or after a few copies with nothing (no unit
     * on screen). A stale record shows nothing: the frame no longer matches. */
    if (!g_used[g_build] && ++empty_copies < 4) return;
    empty_copies = 0;
    if (!g_pub_lock_init) InitializeCriticalSection(&g_pub_lock), g_pub_lock_init = 1;
    EnterCriticalSection(&g_pub_lock);
    g_build ^= 1;                                 /* publish this frame's records */
    g_used[g_build] = 0;
    g_published_frames++;
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (g_last_frame) g_frame_ticks += t.QuadPart - g_last_frame;
        g_last_frame = t.QuadPart;
    }
    LeaveCriticalSection(&g_pub_lock);
}

static uint32_t bgrx(uint16_t p) {
    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

/* The presenter's 2x frame: frame16 is the 1x frame (16-bit, w x h, pitch in
 * bytes), out gets 2w x 2h BGRX. */
void hdvox_compose(const uint8_t* frame16, int pitch, int w, int h, uint32_t* out) {
    static long matched[2], offered[2], logged_at;
    for (int y = 0; y < h; y++) {
        const uint16_t* row = (const uint16_t*)(frame16 + y * pitch);
        uint32_t* o0 = out + (2 * y) * 2 * w, *o1 = o0 + 2 * w;
        for (int x = 0; x < w; x++) {
            uint32_t c = bgrx(row[x]);
            o0[2 * x] = o0[2 * x + 1] = o1[2 * x] = o1[2 * x + 1] = c;
        }
    }
    if (!ra2_vox_hd_on || !g_pub_lock_init) return;
    EnterCriticalSection(&g_pub_lock);
    const uint8_t* p = g_arena[g_build ^ 1];
    size_t end = g_used[g_build ^ 1];
    for (size_t at = 0; at < end;) {
        const rec_t* r = (const rec_t*)(p + at);
        int rw = r->w, rh = r->h;
        const uint16_t* E = (const uint16_t*)(r + 1);
        const uint16_t* O = E + rw * rh;
        const uint8_t* m = (const uint8_t*)(O + 4 * rw * rh);
        for (int j = 0; j < rh; j++) {
            int y = r->y + j;
            if (y < 0 || y >= h) continue;
            const uint16_t* row = (const uint16_t*)(frame16 + y * pitch);
            uint32_t* o0 = out + (2 * y) * 2 * w, *o1 = o0 + 2 * w;
            for (int i = 0; i < rw; i++) {
                int x = r->x + i, k = j * rw + i;
                if (x < 0 || x >= w || !m[k]) continue;
                offered[r->kind]++;
                if (row[x] != E[k]) continue;      /* drawn over since */
                matched[r->kind]++;
                o0[2 * x] = bgrx(O[k * 4]), o0[2 * x + 1] = bgrx(O[k * 4 + 1]);
                o1[2 * x] = bgrx(O[k * 4 + 2]), o1[2 * x + 1] = bgrx(O[k * 4 + 3]);
            }
        }
        at += (sizeof(rec_t) + (size_t)rw * rh * 11 + 7) & ~(size_t)7;
    }
    LeaveCriticalSection(&g_pub_lock);
    if (g_published_frames - logged_at >= 300) {
        logged_at = g_published_frames;
        LARGE_INTEGER hz;
        QueryPerformanceFrequency(&hz);
        double n = g_published_frames > 1 ? (double)(g_published_frames - 1) : 1.0;
        fprintf(stderr, "[hdvox] %ld frames, %ld records (%ld shadows, %ld aircraft parts, %ld shadows not on the battlefield); "
                "2x pixels shown: units %ld of %ld, shadows %ld of %ld; "
                "%.1f ms a frame, %.2f of it the 2x renders (%ld renders, %ld from memory, %ld voxel animation renders)\n",
                g_published_frames, g_records, g_sh_records, g_direct,
                g_sh_skipped, matched[0], offered[0], matched[1], offered[1],
                1000.0 * g_frame_ticks / hz.QuadPart / n, 1000.0 * g_hd_ticks / hz.QuadPart / n,
                g_memo_hits + g_memo_misses, g_memo_hits, g_anim_renders);
    }
}
